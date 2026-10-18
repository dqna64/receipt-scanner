#include "ReceiptService.h"

#include "auth/TokenUtils.h"
#include "config/Env.h"
#include "receipts/ItemService.h"
#include "receipts/RowValues.h"

#include <drogon/drogon.h>
#include <sodium.h>

namespace receipt_scanner::receipts {

namespace {

// Same local-SHA256-hex pattern as auth/TokenUtils.cpp and storage/Sigv4Signer.cpp -- a
// handful of crypto helpers, each small enough that sharing one utility across modules
// would be more machinery than the three near-identical implementations it replaces.
std::string sha256Hex(const std::string &data) {
  unsigned char digest[crypto_hash_sha256_BYTES];
  crypto_hash_sha256(digest, reinterpret_cast<const unsigned char *>(data.data()), data.size());
  static const char *hex = "0123456789abcdef";
  std::string out;
  out.reserve(crypto_hash_sha256_BYTES * 2);
  for (unsigned char byte : digest) {
    out.push_back(hex[byte >> 4]);
    out.push_back(hex[byte & 0x0F]);
  }
  return out;
}

// MAX_IMAGES_PER_RECEIPT (default 8): the add-time cap on a receipt's image count (decided 1a).
int maxImagesPerReceipt() {
  return std::stoi(receipt_scanner::config::envOr("MAX_IMAGES_PER_RECEIPT", "8"));
}

// A fresh, unguessable S3 key: receipts/<user>/<receipt>/<random>.webp.
std::string generateImageKey(int64_t userId, int64_t receiptId) {
  return "receipts/" + std::to_string(userId) + "/" + std::to_string(receiptId) + "/" +
         auth::TokenUtils::generateToken(16) + ".webp";
}

using namespace row_values;

// ISO-8601 UTC (spec API conventions), independent of the server's TimeZone setting.
std::string isoUtc(const std::string &column) {
  return "to_char(" + column + " AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.MS\"Z\"') AS " + column;
}

// Reads through v_receipt_state for needs_review/is_confirmed. NUMERIC fx_rate_to_aud as float8.
const std::string &receiptSelect() {
  static const std::string sql =
      "SELECT id, store_name_raw, merchant, purchase_date::text AS purchase_date, total_cents, direction, currency, "
      "kind, scan_state, tax_deductible, note, source, payment_method_id, offsets_receipt_id, "
      "fx_rate_to_aud::float8 AS fx_rate_to_aud, aud_equivalent_cents, " +
      isoUtc("proposed_at") + ", " + isoUtc("reviewed_at") + ", needs_review, is_confirmed, " + isoUtc("created_at") +
      ", " + isoUtc("updated_at") + " FROM v_receipt_state";
  return sql;
}

// A row selected with receiptSelect() -> ReceiptSummary.
ReceiptSummary rowToSummary(const drogon::orm::Row &row) {
  return ReceiptSummary{
      row["id"].as<int64_t>(),
      optString(row, "store_name_raw"),
      optString(row, "merchant"),
      optString(row, "purchase_date"),
      row["total_cents"].as<int64_t>(),
      row["direction"].as<std::string>(),
      row["currency"].as<std::string>(),
      row["kind"].as<std::string>(),
      row["scan_state"].as<std::string>(),
      row["tax_deductible"].as<bool>(),
      optString(row, "note"),
      row["source"].as<std::string>(),
      optInt(row, "payment_method_id"),
      optInt(row, "offsets_receipt_id"),
      optDouble(row, "fx_rate_to_aud"),
      optInt(row, "aud_equivalent_cents"),
      optString(row, "proposed_at"),
      optString(row, "reviewed_at"),
      row["needs_review"].as<bool>(),
      row["is_confirmed"].as<bool>(),
      row["created_at"].as<std::string>(),
      row["updated_at"].as<std::string>(),
  };
}

// The current editable fields of a receipt row -- updateReceipt's starting point for the patch.
EditableReceipt rowToEditable(const drogon::orm::Row &row) {
  EditableReceipt r;
  r.kind = row["kind"].as<std::string>();
  r.direction = row["direction"].as<std::string>();
  r.totalCents = row["total_cents"].as<int64_t>();
  r.currency = row["currency"].as<std::string>();
  r.merchant = optString(row, "merchant");
  r.storeNameRaw = optString(row, "store_name_raw");
  r.purchaseDate = optString(row, "purchase_date");
  r.paymentMethodId = optInt(row, "payment_method_id");
  r.taxDeductible = row["tax_deductible"].as<bool>();
  r.note = optString(row, "note");
  r.offsetsReceiptId = optInt(row, "offsets_receipt_id");
  r.fxRateToAud = optDouble(row, "fx_rate_to_aud");
  return r;
}

} // namespace

drogon::Task<std::vector<ReceiptService::PreparedImage>> ReceiptService::prepareImages(
    const std::vector<std::string> &rawImages) {
  std::vector<PreparedImage> prepared;
  prepared.reserve(rawImages.size());

  for (const auto &raw : rawImages) {
    // Undecodable bytes (not JPEG/PNG/WebP, e.g. HEIC, which the pinned libvips build has no
    // decoder for) throw storage::ImageDecodeError straight out to the caller; the
    // controller translates it to a 400.
    auto normalized = co_await normalizer_.normalize(raw);
    auto hash = sha256Hex(normalized.bytes);
    prepared.push_back(PreparedImage{std::move(normalized), std::move(hash)});
  }

  co_return prepared;
}

drogon::Task<> ReceiptService::lockUserImages(const std::shared_ptr<drogon::orm::Transaction> &transaction,
                                              int64_t userId) {
  // Held until the transaction ends, so two concurrent writes for the same user (a double-tap
  // upload) can't both pass the dedup/cap checks before either commits. Per-user rather than
  // per-hash: a batch has several hashes, and taking several locks would need a fixed order to
  // avoid deadlock. Keyed on the bare user id -- the only advisory lock in the app so far.
  co_await transaction->execSqlCoro("SELECT pg_advisory_xact_lock($1::bigint)", userId);
}

drogon::Task<std::optional<ImageConflict>> ReceiptService::findConflict(
    const std::shared_ptr<drogon::orm::Transaction> &transaction, int64_t userId,
    const std::vector<PreparedImage> &prepared) {
  for (const auto &prep : prepared) {
    auto dup = co_await transaction->execSqlCoro(
        "SELECT id, receipt_id FROM receipt_images WHERE user_id = $1 AND image_sha256 = $2 LIMIT 1", userId,
        prep.sha256Hex);
    if (!dup.empty()) {
      co_return ImageConflict{dup[0]["receipt_id"].as<int64_t>(), dup[0]["id"].as<int64_t>()};
    }
  }
  co_return std::nullopt;
}

drogon::Task<> ReceiptService::storeImages(const std::shared_ptr<drogon::orm::Transaction> &transaction,
                                           int64_t userId, int64_t receiptId,
                                           const std::vector<PreparedImage> &prepared) {
  // S3 and Postgres aren't transactionally joined. S3 PUT before the DB insert that references
  // it, so a failure (the caller rolls back) leaves at worst orphaned S3 objects -- wasted
  // storage -- never a DB row pointing at an image that was never written.
  for (const auto &prep : prepared) {
    auto key = generateImageKey(userId, receiptId);
    co_await imageStore_.put(key, "image/webp", prep.normalized.bytes);
    co_await transaction->execSqlCoro(
        "INSERT INTO receipt_images (receipt_id, user_id, image_key, image_sha256) VALUES ($1, $2, $3, $4)",
        receiptId, userId, key, prep.sha256Hex);
  }
}

drogon::Task<std::variant<int64_t, ImageConflict>> ReceiptService::uploadReceipt(int64_t userId,
                                                                                  std::vector<std::string> images,
                                                                                  bool force) {
  if (static_cast<int64_t>(images.size()) > maxImagesPerReceipt()) {
    throw TooManyImagesError();
  }
  auto prepared = co_await prepareImages(images);

  const std::string provisionalKind(kDefaultKind);
  const std::string provisionalDirection(*rulesFor(kDefaultKind).canonicalDirection);
  auto transaction = co_await db_->newTransactionCoro();
  // Explicit rollback on every non-success exit: Drogon's Transaction commits in its
  // destructor unless rolled back, so an exception (e.g. S3 failing on image 3 of 3) would
  // otherwise commit the receipt with images 1-2. Same reasoning as AuthService::registerUser.
  try {
    co_await lockUserImages(transaction, userId);
    if (!force) {
      auto conflict = co_await findConflict(transaction, userId, prepared);
      if (conflict) {
        transaction->rollback();
        co_return *conflict;
      }
    }

    // Provisional kind/direction (spec: "receipts.kind: purchase (default)") -- Step 10's
    // scanner overwrites these once it classifies the actual document type.
    auto receiptResult = co_await transaction->execSqlCoro(
        "INSERT INTO receipts (user_id, kind, direction, source, scan_state) "
        "VALUES ($1, $2, $3, 'scanned', 'uploaded') RETURNING id",
        userId, provisionalKind, provisionalDirection);
    auto receiptId = receiptResult[0]["id"].as<int64_t>();

    co_await storeImages(transaction, userId, receiptId, prepared);
    co_return receiptId;
  } catch (...) {
    transaction->rollback();
    throw;
  }
}

drogon::Task<std::variant<std::monostate, ImageConflict>> ReceiptService::addImages(int64_t userId,
                                                                                     int64_t receiptId,
                                                                                     std::vector<std::string> images,
                                                                                     bool force) {
  auto owned = co_await db_->execSqlCoro("SELECT 1 FROM receipts WHERE id = $1 AND user_id = $2", receiptId, userId);
  if (owned.empty()) {
    throw ReceiptNotFoundError();
  }

  auto prepared = co_await prepareImages(images);

  auto transaction = co_await db_->newTransactionCoro();
  try { // rollback on every non-success exit -- see uploadReceipt
    co_await lockUserImages(transaction, userId);

    // Counted under the lock, so two concurrent adds can't both fit under the cap.
    auto countResult =
        co_await transaction->execSqlCoro("SELECT count(*) AS n FROM receipt_images WHERE receipt_id = $1", receiptId);
    auto currentCount = countResult[0]["n"].as<int64_t>();
    if (currentCount + static_cast<int64_t>(prepared.size()) > maxImagesPerReceipt()) {
      throw TooManyImagesError();
    }

    if (!force) {
      auto conflict = co_await findConflict(transaction, userId, prepared);
      if (conflict) {
        transaction->rollback();
        co_return *conflict;
      }
    }

    co_await storeImages(transaction, userId, receiptId, prepared);
    // Sets the receipt up for the Step 10 worker to (re-)scan the FULL image set once it
    // exists; until then this just parks at scan_state=uploaded, per spec (explicitly
    // expected: "until Step 10 lands it just parks there").
    co_await transaction->execSqlCoro("UPDATE receipts SET scan_state = 'uploaded' WHERE id = $1", receiptId);
    co_return std::monostate{};
  } catch (...) {
    transaction->rollback();
    throw;
  }
}

drogon::Task<> ReceiptService::validateReferences(const std::shared_ptr<drogon::orm::Transaction> &transaction,
                                                  int64_t userId, std::optional<int64_t> receiptId,
                                                  const EditableReceipt &receipt) {
  if (receipt.paymentMethodId) {
    auto owned = co_await transaction->execSqlCoro("SELECT 1 FROM payment_methods WHERE id = $1 AND user_id = $2",
                                                   *receipt.paymentMethodId, userId);
    if (owned.empty()) {
      throw ValidationError("payment_method_id does not exist");
    }
  }
  if (receipt.offsetsReceiptId) {
    if (receiptId && *receipt.offsetsReceiptId == *receiptId) {
      throw ValidationError("a receipt cannot offset itself");
    }
    auto target = co_await transaction->execSqlCoro("SELECT kind FROM receipts WHERE id = $1 AND user_id = $2",
                                                    *receipt.offsetsReceiptId, userId);
    if (target.empty()) {
      throw ValidationError("offsets_receipt_id does not exist");
    }
    const auto targetKind = target[0]["kind"].as<std::string>();
    if (!rulesFor(targetKind).isOffsetTarget) {
      throw ValidationError("offsets_receipt_id can't point at a " + targetKind);
    }
  }
}

drogon::Task<int64_t> ReceiptService::createManual(int64_t userId, const nlohmann::json &body) {
  auto input = parseManualEntry(body);
  const auto &r = input.receipt;
  const auto audEquivalent = audEquivalentCents(r.totalCents, r.fxRateToAud);

  auto transaction = co_await db_->newTransactionCoro();
  try { // rollback on every non-success exit -- see uploadReceipt
    co_await validateReferences(transaction, userId, std::nullopt, r);

    // Born reviewed: reviewed_at set, proposed_at NULL -> is_confirmed immediately (spec Capture).
    // fx_rate_to_aud is NUMERIC; ::float8 matches the double Drogon binds.
    auto result = co_await transaction->execSqlCoro(
        "INSERT INTO receipts (user_id, kind, direction, total_cents, currency, merchant, purchase_date, "
        "payment_method_id, tax_deductible, note, offsets_receipt_id, fx_rate_to_aud, aud_equivalent_cents, "
        "source, scan_state, reviewed_at) "
        "VALUES ($1, $2, $3, $4, $5, $6, COALESCE($7::date, (now() AT TIME ZONE 'Australia/Sydney')::date), "
        "$8, $9, $10, $11, $12::float8, $13, 'manual', 'idle', now()) RETURNING id",
        userId, r.kind, r.direction, r.totalCents, r.currency, r.merchant, r.purchaseDate, r.paymentMethodId,
        r.taxDeductible, r.note, r.offsetsReceiptId, r.fxRateToAud, audEquivalent);
    auto receiptId = result[0]["id"].as<int64_t>();

    if (input.items.empty()) {
      ItemInput implicitItem;
      implicitItem.name = r.merchant;
      implicitItem.amountCents = r.totalCents;
      implicitItem.categoryId = input.categoryId;
      co_await ItemService::insert(transaction, userId, receiptId, r.kind, implicitItem);
    } else {
      for (const auto &item : input.items) {
        co_await ItemService::insert(transaction, userId, receiptId, r.kind, item);
      }
    }
    co_return receiptId;
  } catch (...) {
    transaction->rollback();
    throw;
  }
}

drogon::Task<ReceiptSummary> ReceiptService::updateReceipt(int64_t userId, int64_t receiptId,
                                                           const nlohmann::json &body) {
  auto transaction = co_await db_->newTransactionCoro();
  try { // rollback on every non-success exit -- see uploadReceipt
    auto current = co_await transaction->execSqlCoro(
        "SELECT kind, direction, total_cents, currency, merchant, store_name_raw, purchase_date::text AS purchase_date, "
        "payment_method_id, tax_deductible, note, offsets_receipt_id, fx_rate_to_aud::float8 AS fx_rate_to_aud "
        "FROM receipts WHERE id = $1 AND user_id = $2 FOR UPDATE",
        receiptId, userId);
    if (current.empty()) {
      throw ReceiptNotFoundError();
    }
    auto r = rowToEditable(current[0]);
    const bool wasIncomeSide = rulesFor(r.kind).usesIncomeCategories;
    bool markReviewed = false;
    applyReceiptPatch(r, body, markReviewed);

    co_await validateReferences(transaction, userId, receiptId, r);
    const bool isIncomeSide = rulesFor(r.kind).usesIncomeCategories;
    if (isIncomeSide != wasIncomeSide) {
      auto clash = co_await transaction->execSqlCoro(
          "SELECT 1 FROM items i JOIN categories c ON c.id = i.category_id "
          "WHERE i.receipt_id = $1 AND c.is_income <> $2 LIMIT 1",
          receiptId, isIncomeSide);
      if (!clash.empty()) {
        throw ValidationError("this kind change would leave items in the wrong expense/income categories -- "
                              "recategorize them first");
      }
    }

    const auto audEquivalent = audEquivalentCents(r.totalCents, r.fxRateToAud);
    co_await transaction->execSqlCoro(
        "UPDATE receipts SET kind = $1, direction = $2, total_cents = $3, currency = $4, merchant = $5, "
        "store_name_raw = $6, purchase_date = $7::date, payment_method_id = $8, tax_deductible = $9, note = $10, "
        "offsets_receipt_id = $11, fx_rate_to_aud = $12::float8, aud_equivalent_cents = $13, updated_at = now(), "
        "reviewed_at = CASE WHEN $14 THEN now() ELSE reviewed_at END WHERE id = $15",
        r.kind, r.direction, r.totalCents, r.currency, r.merchant, r.storeNameRaw, r.purchaseDate, r.paymentMethodId,
        r.taxDeductible, r.note, r.offsetsReceiptId, r.fxRateToAud, audEquivalent, markReviewed, receiptId);

    auto updated = co_await transaction->execSqlCoro(receiptSelect() + " WHERE id = $1", receiptId);
    auto summary = rowToSummary(updated[0]);
    co_return summary;
  } catch (...) {
    transaction->rollback();
    throw;
  }
}

drogon::Task<std::optional<ReceiptSummary>> ReceiptService::getReceipt(int64_t userId, int64_t receiptId) {
  auto result = co_await db_->execSqlCoro(receiptSelect() + " WHERE id = $1 AND user_id = $2", receiptId, userId);
  if (result.empty()) {
    co_return std::nullopt;
  }
  co_return rowToSummary(result[0]);
}

drogon::Task<std::vector<ReceiptSummary>> ReceiptService::listReceipts(int64_t userId, int limit, int offset) {
  auto result = co_await db_->execSqlCoro(
      // Bare LIMIT/OFFSET placeholders have no column to infer a type from, so Postgres
      // defaults them to bigint -- explicit ::int casts keep that in sync with the int
      // (4-byte) binary params Drogon actually binds here, avoiding a param-size mismatch.
      receiptSelect() + " WHERE user_id = $1 ORDER BY created_at DESC, id DESC LIMIT $2::int OFFSET $3::int",
      userId, limit, offset);
  std::vector<ReceiptSummary> out;
  out.reserve(result.size());
  for (const auto &row : result) {
    out.push_back(rowToSummary(row));
  }
  co_return out;
}

drogon::Task<std::vector<ImageRef>> ReceiptService::listImages(int64_t userId, int64_t receiptId) {
  auto owned = co_await db_->execSqlCoro("SELECT 1 FROM receipts WHERE id = $1 AND user_id = $2", receiptId, userId);
  if (owned.empty()) {
    throw ReceiptNotFoundError();
  }

  auto result = co_await db_->execSqlCoro(
      "SELECT id, created_at FROM receipt_images WHERE receipt_id = $1 ORDER BY created_at", receiptId);
  std::vector<ImageRef> out;
  out.reserve(result.size());
  for (const auto &row : result) {
    out.push_back(ImageRef{row["id"].as<int64_t>(), row["created_at"].as<std::string>()});
  }
  co_return out;
}

drogon::Task<bool> ReceiptService::deleteReceipt(int64_t userId, int64_t receiptId) {
  // Ownership-scoped from the start (the JOIN), so this never reads another user's image
  // keys even transiently -- not just the final DELETE below.
  auto images = co_await db_->execSqlCoro(
      "SELECT ri.image_key FROM receipt_images ri JOIN receipts r ON r.id = ri.receipt_id "
      "WHERE ri.receipt_id = $1 AND r.user_id = $2",
      receiptId, userId);

  auto result =
      co_await db_->execSqlCoro("DELETE FROM receipts WHERE id = $1 AND user_id = $2 RETURNING id", receiptId, userId);
  if (result.empty()) {
    co_return false;
  }

  for (const auto &row : images) {
    try {
      co_await imageStore_.remove(row["image_key"].as<std::string>());
    } catch (const std::exception &e) {
      LOG_WARN << "ReceiptService::deleteReceipt: failed to remove S3 object " << row["image_key"].as<std::string>()
               << " after the DB row was already deleted: " << e.what();
    }
  }

  co_return true;
}

drogon::Task<std::optional<ImageLocation>> ReceiptService::findImage(int64_t imageId) {
  auto result = co_await db_->execSqlCoro(
      "SELECT ri.image_key, r.user_id FROM receipt_images ri JOIN receipts r ON r.id = ri.receipt_id "
      "WHERE ri.id = $1",
      imageId);
  if (result.empty()) {
    co_return std::nullopt;
  }
  co_return ImageLocation{result[0]["user_id"].as<int64_t>(), result[0]["image_key"].as<std::string>()};
}

} // namespace receipt_scanner::receipts
