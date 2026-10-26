#include "ScanWorker.h"

#include "config/Env.h"
#include "receipts/Errors.h"
#include "receipts/ItemService.h"
#include "receipts/JsonInput.h"
#include "receipts/Kinds.h"
#include "receipts/ReceiptStates.h"
#include "receipts/RowValues.h"

#include <drogon/drogon.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <optional>

namespace receipt_scanner::scan {

namespace {

using receipts::row_values::optInt;
using receipts::row_values::optString;
namespace scan_state = receipts::scan_state;

struct CategoryRef {
  int64_t id;
  std::string name;
  bool isIncome;
};

struct TagRef {
  int64_t id;
  std::string name;
};

// The user's own category and tag lists: the provider's enums, and the name -> id map for the
// items it returns (spec Invariants: the scanner's enums come from the requesting user's taxonomy).
struct Taxonomy {
  std::vector<CategoryRef> categories;
  std::vector<TagRef> tags;
};

// The receipt as the write step needs it. Read without a lock to prepare the verify
// comparison, then again under FOR UPDATE to check nothing changed in between.
struct Snapshot {
  bool reviewed;
  std::string kind;
  int64_t totalCents;
  std::string currency;
  std::optional<std::string> merchant;
  std::optional<std::string> purchaseDate;
  bool taxDeductible;
  std::optional<int64_t> offsetsReceiptId;
  std::string updatedAt;
  int64_t itemCount;
};

std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
  return s;
}

int envInt(const char *name, int fallback) {
  return std::stoi(config::envOr(name, std::to_string(fallback)));
}

// Days from `from` to `to`, both YYYY-MM-DD (already validated).
int daysBetween(const std::string &from, const std::string &to) {
  auto parse = [](const std::string &s) {
    return std::chrono::sys_days{std::chrono::year{std::stoi(s.substr(0, 4))} /
                                 std::chrono::month{static_cast<unsigned>(std::stoi(s.substr(5, 2)))} /
                                 std::chrono::day{static_cast<unsigned>(std::stoi(s.substr(8, 2)))}};
  };
  return static_cast<int>((parse(to) - parse(from)).count());
}

// Rejects a result the write step couldn't store, as a ScanError (spends an attempt, same as a
// parse failure -- the provider returned something unusable). Uppercases the currency.
void validateResult(ScanResult &result) {
  const auto *rules = receipts::findKind(result.kind);
  if (!rules || !rules->canonicalDirection) {
    throw ScanError("provider returned an unusable kind: " + result.kind);
  }
  if (result.totalCents < 0) {
    throw ScanError("provider returned a negative total");
  }
  for (const auto &item : result.items) {
    if (item.amountCents < 0 || (item.unitPriceCents && *item.unitPriceCents < 0) ||
        (item.quantity && !(*item.quantity > 0))) {
      throw ScanError("provider returned an item with a negative amount or non-positive quantity");
    }
  }
  try {
    result.currency = receipts::json_input::currency(nlohmann::json(result.currency), "currency");
    if (result.purchaseDate) {
      receipts::json_input::date(nlohmann::json(*result.purchaseDate), "purchase_date");
    }
  } catch (const receipts::ValidationError &e) {
    throw ScanError(std::string("provider returned ") + e.what());
  }
}

// Client taken by value: a Transaction converts to a temporary DbClientPtr (see ItemService.cpp).
drogon::Task<std::optional<Snapshot>> readSnapshot(drogon::orm::DbClientPtr db, const ScanWorker::Claim &claim,
                                                   bool forUpdate) {
  auto result = co_await db->execSqlCoro(
      std::string("SELECT reviewed_at IS NOT NULL AS reviewed, kind, total_cents, currency, merchant, "
                  "purchase_date::text AS purchase_date, tax_deductible, offsets_receipt_id, "
                  "updated_at::text AS updated_at, "
                  "(SELECT count(*) FROM items i WHERE i.receipt_id = r.id) AS item_count "
                  "FROM receipts r WHERE r.id = $1 AND r.scan_state = $2 AND r.scan_started_at = $3::timestamptz") +
          (forUpdate ? " FOR UPDATE" : ""),
      claim.receiptId, std::string(scan_state::kScanning), claim.claimedAt);
  if (result.empty()) {
    co_return std::nullopt;
  }
  const auto &row = result[0];
  co_return Snapshot{
      row["reviewed"].as<bool>(),
      row["kind"].as<std::string>(),
      row["total_cents"].as<int64_t>(),
      row["currency"].as<std::string>(),
      optString(row, "merchant"),
      optString(row, "purchase_date"),
      row["tax_deductible"].as<bool>(),
      optInt(row, "offsets_receipt_id"),
      row["updated_at"].as<std::string>(),
      row["item_count"].as<int64_t>(),
  };
}

drogon::Task<Taxonomy> loadTaxonomy(drogon::orm::DbClientPtr db, int64_t userId) {
  Taxonomy taxonomy;
  auto categories =
      co_await db->execSqlCoro("SELECT id, name, is_income FROM categories WHERE user_id = $1 ORDER BY id", userId);
  for (const auto &row : categories) {
    taxonomy.categories.push_back(
        {row["id"].as<int64_t>(), row["name"].as<std::string>(), row["is_income"].as<bool>()});
  }
  auto tags = co_await db->execSqlCoro("SELECT id, name FROM tags WHERE user_id = $1 ORDER BY id", userId);
  for (const auto &row : tags) {
    taxonomy.tags.push_back({row["id"].as<int64_t>(), row["name"].as<std::string>()});
  }
  co_return taxonomy;
}

ScanContext makeContext(const Taxonomy &taxonomy, std::optional<std::string> knownKind) {
  ScanContext context;
  context.knownKind = std::move(knownKind);
  for (const auto &category : taxonomy.categories) {
    (category.isIncome ? context.incomeCategories : context.expenseCategories).push_back(category.name);
  }
  for (const auto &tag : taxonomy.tags) {
    context.tags.push_back(tag.name);
  }
  return context;
}

// A scanned item -> ItemInput, resolving names case-insensitively against the user's lists. A
// category name from the wrong expense/income side for `kind`, or not on the list at all, leaves
// the item uncategorized rather than failing the scan; unknown tag names are dropped.
receipts::ItemInput toItemInput(const ScannedItem &scanned, const std::string &kind, const Taxonomy &taxonomy) {
  receipts::ItemInput input;
  input.name = scanned.name;
  input.quantity = scanned.quantity;
  input.unitPriceCents = scanned.unitPriceCents;
  input.amountCents = scanned.amountCents;
  input.informational = scanned.informational;
  if (scanned.category) {
    const bool incomeSide = receipts::rulesFor(kind).usesIncomeCategories;
    for (const auto &category : taxonomy.categories) {
      if (category.isIncome == incomeSide && lower(category.name) == lower(*scanned.category)) {
        input.categoryId = category.id;
        break;
      }
    }
  }
  for (const auto &tagName : scanned.tags) {
    for (const auto &tag : taxonomy.tags) {
      if (lower(tag.name) == lower(tagName) &&
          std::find(input.tagIds.begin(), input.tagIds.end(), tag.id) == input.tagIds.end()) {
        input.tagIds.push_back(tag.id);
      }
    }
  }
  return input;
}

drogon::Task<> insertScannedItems(const std::shared_ptr<drogon::orm::Transaction> &transaction,
                                  const ScanWorker::Claim &claim, const std::string &kind,
                                  const std::vector<ScannedItem> &items, const Taxonomy &taxonomy) {
  for (const auto &scanned : items) {
    auto input = toItemInput(scanned, kind, taxonomy);
    co_await receipts::ItemService::insert(transaction, claim.userId, claim.receiptId, kind, input);
  }
}

// Extraction mode (row never reviewed, so nothing in it is trusted): the scan's fields and items
// replace the row's. Fields the scan doesn't cover but that its kind constrains are reconciled
// so the row stays valid: tax_deductible is cleared unless the scanned kind allows it (it's also
// a DB CHECK), offsets_receipt_id unless the kind can offset.
drogon::Task<> writeExtraction(const std::shared_ptr<drogon::orm::Transaction> &transaction,
                               const ScanWorker::Claim &claim, const ScanResult &result, const Taxonomy &taxonomy,
                               const Snapshot &snapshot) {
  const auto &rules = receipts::rulesFor(result.kind);
  const std::string direction(*rules.canonicalDirection);
  const bool taxDeductible = snapshot.taxDeductible && rules.allowsTaxDeductible;
  const auto offsets = rules.canOffset ? snapshot.offsetsReceiptId : std::nullopt;
  co_await transaction->execSqlCoro(
      "UPDATE receipts SET kind = $1, direction = $2, total_cents = $3, currency = $4, merchant = $5, "
      "store_name_raw = $6, purchase_date = $7::date, tax_deductible = $8, offsets_receipt_id = $9, "
      "raw_ocr_json = $10::jsonb, scan_state = $11, scan_started_at = NULL, proposed_at = now(), "
      "updated_at = now() WHERE id = $12",
      result.kind, direction, result.totalCents, result.currency, result.merchant, result.storeNameRaw,
      result.purchaseDate, taxDeductible, offsets, result.raw.dump(), std::string(scan_state::kIdle), claim.receiptId);
  co_await transaction->execSqlCoro("DELETE FROM items WHERE receipt_id = $1", claim.receiptId);
  co_await insertScannedItems(transaction, claim, result.kind, result.items, taxonomy);
}

// Verify mode (row reviewed, so its fields are the user's): never overwrite -- fill only what's
// empty (merchant, store name, date, and items if there are none), keep the row's kind. allMatch
// re-stamps reviewed_at alongside proposed_at so the row stays confirmed with no queue trip;
// otherwise proposed_at now leads and the row derives as needs_review (spec Recurring, verify-scan).
drogon::Task<> writeVerify(const std::shared_ptr<drogon::orm::Transaction> &transaction,
                           const ScanWorker::Claim &claim, const ScanResult &result, const Taxonomy &taxonomy,
                           const Snapshot &snapshot, bool allMatch) {
  co_await transaction->execSqlCoro(
      "UPDATE receipts SET merchant = COALESCE(merchant, $1), store_name_raw = COALESCE(store_name_raw, $2), "
      "purchase_date = COALESCE(purchase_date, $3::date), raw_ocr_json = $4::jsonb, scan_state = $5, "
      "scan_started_at = NULL, proposed_at = now(), reviewed_at = CASE WHEN $6 THEN now() ELSE reviewed_at END, "
      "updated_at = now() WHERE id = $7",
      result.merchant, result.storeNameRaw, result.purchaseDate, result.raw.dump(), std::string(scan_state::kIdle),
      allMatch, claim.receiptId);
  if (snapshot.itemCount == 0) {
    co_await insertScannedItems(transaction, claim, snapshot.kind, result.items, taxonomy);
  }
}

} // namespace

ScanWorkerConfig ScanWorkerConfig::fromEnv() {
  ScanWorkerConfig defaults;
  ScanWorkerConfig config;
  config.concurrency = envInt("SCAN_CONCURRENCY", defaults.concurrency);
  config.maxScanAttempts = envInt("SCAN_MAX_ATTEMPTS", defaults.maxScanAttempts);
  config.maxReclaims = envInt("SCAN_MAX_RECLAIMS", defaults.maxReclaims);
  config.reclaimAfterSeconds = envInt("SCAN_RECLAIM_SECONDS", defaults.reclaimAfterSeconds);
  config.matchDateDays = envInt("MATCH_DATE_DAYS", defaults.matchDateDays);
  config.merchantMatchThreshold =
      std::stod(config::envOr("MERCHANT_MATCH_THRESHOLD", std::to_string(defaults.merchantMatchThreshold)));
  return config;
}

ScanWorker::ScanWorker(drogon::orm::DbClientPtr db, storage::ImageStore &imageStore, ScanProvider &provider,
                       MerchantMatcher &merchantMatcher, notify::Notifier &notifier, ScanWorkerConfig config)
    : db_(std::move(db)), imageStore_(imageStore), provider_(provider), merchantMatcher_(merchantMatcher),
      notifier_(notifier), config_(config) {}

drogon::Task<int> ScanWorker::tick() {
  int claimed = 0;
  try {
    co_await reclaimStuck();
    auto claims = co_await claim();
    claimed = static_cast<int>(claims.size());
    // Concurrently, so one hung scan doesn't hold up the others claimed alongside it.
    std::vector<drogon::Task<>> scans;
    for (const auto &c : claims) {
      scans.push_back(process(c)); // never throws
    }
    co_await drogon::when_all(std::move(scans));
  } catch (const std::exception &e) {
    LOG_ERROR << "ScanWorker::tick: " << e.what();
  }
  co_return claimed;
}

// A scan stuck in `scanning` past the timeout died with its process (or hung): re-queue it,
// spending reclaim_count, never scan_attempts (spec Invariants: deploy churn must not fail a
// good receipt). Only the high backstop sends it to scan_failed.
drogon::Task<> ScanWorker::reclaimStuck() {
  auto rows = co_await db_->execSqlCoro(
      "UPDATE receipts SET reclaim_count = reclaim_count + 1, scan_started_at = NULL, "
      "scan_state = CASE WHEN reclaim_count + 1 >= $1::int THEN $2::text ELSE $3::text END "
      "WHERE scan_state = $4 AND scan_started_at < now() - $5::int * interval '1 second' "
      "RETURNING id, user_id, scan_state",
      config_.maxReclaims, std::string(scan_state::kScanFailed), std::string(scan_state::kUploaded),
      std::string(scan_state::kScanning), config_.reclaimAfterSeconds);
  for (const auto &row : rows) {
    LOG_WARN << "ScanWorker: reclaimed stuck scan of receipt " << row["id"].as<int64_t>();
    if (row["scan_state"].as<std::string>() == scan_state::kScanFailed) {
      co_await notifyScanFailed(row["user_id"].as<int64_t>(), row["id"].as<int64_t>(),
                                "the scan kept timing out");
    }
  }
}

// Atomic claim (spec): SKIP LOCKED means two workers -- or two overlapping ticks -- can never
// take the same row. clock_timestamp() rather than now(), so the claim token is the actual time.
// Free slots are counted from the rows in `scanning`, not an in-process counter: a hung scan
// holds its slot only until reclaimStuck re-queues its row. Two claims racing can each see the
// same count, so the cap is soft by at most one tick's worth of claims.
drogon::Task<std::vector<ScanWorker::Claim>> ScanWorker::claim() {
  auto rows = co_await db_->execSqlCoro(
      "UPDATE receipts SET scan_state = $1, scan_started_at = clock_timestamp() "
      "WHERE id IN (SELECT id FROM receipts WHERE scan_state = $2 ORDER BY created_at, id "
      "LIMIT GREATEST($3::int - (SELECT count(*) FROM receipts WHERE scan_state = $1), 0) "
      "FOR UPDATE SKIP LOCKED) "
      "RETURNING id, user_id, scan_started_at::text AS claimed_at",
      std::string(scan_state::kScanning), std::string(scan_state::kUploaded), config_.concurrency);
  std::vector<Claim> claims;
  for (const auto &row : rows) {
    claims.push_back({row["id"].as<int64_t>(), row["user_id"].as<int64_t>(), row["claimed_at"].as<std::string>()});
  }
  co_return claims;
}

// Scans one claimed receipt end to end. Never throws: every failure is recorded against the
// receipt's budgets instead (ScanError -> scan_attempts, anything else -> reclaim_count).
drogon::Task<> ScanWorker::process(Claim claim) {
  bool failed = false;
  bool scanError = false;
  bool terminal = false;
  std::string error;
  try {
    auto snapshot = co_await readSnapshot(db_, claim, /*forUpdate=*/false);
    if (snapshot) {
      auto keys = co_await db_->execSqlCoro(
          "SELECT image_key FROM receipt_images WHERE receipt_id = $1 ORDER BY created_at, id", claim.receiptId);
      if (keys.empty()) {
        terminal = true; // retrying can't produce images
        throw ScanError("receipt has no images");
      }
      std::vector<std::string> images;
      for (const auto &row : keys) {
        auto bytes = co_await imageStore_.get(row["image_key"].as<std::string>());
        images.push_back(std::move(bytes));
      }
      auto taxonomy = co_await loadTaxonomy(db_, claim.userId);
      auto context =
          makeContext(taxonomy, snapshot->reviewed ? std::optional<std::string>(snapshot->kind) : std::nullopt);
      auto result = co_await provider_.scanReceipt(std::move(images), std::move(context));
      validateResult(result);
      co_await applyResult(claim, result);
    }
  } catch (const ScanError &e) {
    failed = scanError = true;
    error = e.what();
  } catch (const std::exception &e) {
    failed = true;
    error = e.what();
  } catch (...) {
    failed = true;
    error = "non-std exception";
  }

  if (failed) {
    try {
      co_await recordFailure(claim, scanError, terminal, error);
    } catch (const std::exception &e) {
      // Left in `scanning`: reclaimStuck re-queues it after the timeout.
      LOG_ERROR << "ScanWorker: couldn't record failure for receipt " << claim.receiptId << ": " << e.what();
    }
  }
}

// Writes a validated result, choosing extraction vs verify from the row as it is NOW, not at
// claim time (the user may have reviewed it mid-scan). Verify's comparison runs before the write
// transaction -- the matcher needs its own query -- so the write re-reads the row under lock and
// starts over if it changed in between. Silently does nothing if the claim went stale meanwhile:
// the row left `scanning` (re-queued by reclaimStuck, or images were added), a later tick claimed
// it again (new scan_started_at), or it was deleted.
drogon::Task<> ScanWorker::applyResult(const Claim &claim, const ScanResult &result) {
  auto taxonomy = co_await loadTaxonomy(db_, claim.userId);
  for (int attempt = 0; attempt < 2; ++attempt) {
    auto before = co_await readSnapshot(db_, claim, /*forUpdate=*/false);
    if (!before) {
      co_return;
    }

    bool allMatch = true;
    if (before->reviewed) {
      // Total is exact, and only meaningful in the same currency; date and merchant are
      // compared only when the row has one (an empty field is filled, not compared).
      allMatch = result.totalCents == before->totalCents && result.currency == before->currency;
      if (before->purchaseDate && result.purchaseDate &&
          std::abs(daysBetween(*before->purchaseDate, *result.purchaseDate)) > config_.matchDateDays) {
        allMatch = false;
      }
      if (before->merchant && result.merchant) {
        auto similarity = co_await merchantMatcher_.similarity(*before->merchant, *result.merchant);
        if (similarity < config_.merchantMatchThreshold) {
          allMatch = false;
        }
      }
    }

    bool written = false;
    auto transaction = co_await db_->newTransactionCoro();
    try { // rollback on every non-success exit -- Drogon's Transaction commits in its destructor
      auto locked = co_await readSnapshot(transaction, claim, /*forUpdate=*/true);
      if (!locked) {
        transaction->rollback();
        co_return;
      }
      if (locked->updatedAt == before->updatedAt && locked->reviewed == before->reviewed &&
          locked->itemCount == before->itemCount) {
        if (locked->reviewed) {
          co_await writeVerify(transaction, claim, result, taxonomy, *locked, allMatch);
        } else {
          co_await writeExtraction(transaction, claim, result, taxonomy, *locked);
        }
        written = true;
      } else {
        transaction->rollback();
      }
    } catch (...) {
      transaction->rollback();
      throw;
    }
    if (written) {
      co_return;
    }
  }
  throw std::runtime_error("receipt kept changing while its scan result was being written");
}

// Spends one unit of the matching budget and re-queues, or moves to scan_failed once the
// budget is exhausted (or at once if `terminal`). Terminal failure touches neither timestamp, so a reviewed receipt stays
// reviewed (spec: an attach glitch can never un-confirm a transaction).
drogon::Task<> ScanWorker::recordFailure(const Claim &claim, bool scanError, bool terminal, const std::string &error) {
  const char *counter = scanError ? "scan_attempts" : "reclaim_count";
  const int cap = terminal ? 0 : scanError ? config_.maxScanAttempts : config_.maxReclaims;
  LOG_WARN << "ScanWorker: scan of receipt " << claim.receiptId << " failed (" << counter << "): " << error;
  auto rows = co_await db_->execSqlCoro(
      std::string("UPDATE receipts SET ") + counter + " = " + counter + " + 1, scan_started_at = NULL, " +
          "scan_state = CASE WHEN " + counter + " + 1 >= $1::int THEN $2::text ELSE $3::text END " +
          "WHERE id = $4 AND scan_state = $5 AND scan_started_at = $6::timestamptz RETURNING scan_state",
      cap, std::string(scan_state::kScanFailed), std::string(scan_state::kUploaded), claim.receiptId,
      std::string(scan_state::kScanning), claim.claimedAt);
  if (!rows.empty() && rows[0]["scan_state"].as<std::string>() == scan_state::kScanFailed) {
    co_await notifyScanFailed(claim.userId, claim.receiptId, error);
  }
}

// Spec Notifications trigger (b): a receipt reaching terminal scan_failed.
drogon::Task<> ScanWorker::notifyScanFailed(int64_t userId, int64_t receiptId, const std::string &reason) {
  nlohmann::json payload = nlohmann::json::object();
  payload["receipt_id"] = receiptId;
  payload["reason"] = reason;
  try {
    co_await notifier_.notify(userId, "scan_failed", payload);
  } catch (const std::exception &e) {
    LOG_ERROR << "ScanWorker: notify failed for receipt " << receiptId << ": " << e.what();
  }
}

} // namespace receipt_scanner::scan
