// Manual entry, receipt PATCH, items, and reference-data CRUD.
//
// GCC-safety conventions for this file (see check-test-patterns.sh, and receipt_test.cpp's
// oneImage for the GCC 12/13 internal compiler error): request
// bodies are parsed from raw strings into named locals before being passed to a co_await'd
// call -- never a braced json literal inside a co_await expression -- and no co_await ever sits
// inside a Catch2 macro.

#include "auth/AuthService.h"
#include "config/Env.h"
#include "receipts/ItemService.h"
#include "receipts/Kinds.h"
#include "receipts/ReceiptStates.h"
#include "receipts/ReceiptService.h"
#include "receipts/ReferenceDataService.h"
#include "storage/S3ImageStore.h"

#include <catch2/catch_test_macros.hpp>
#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>
#include <nlohmann/json.hpp>
#include <trantor/net/EventLoopThread.h>

#include <regex>
#include <set>

using receipt_scanner::config::envOr;
using receipt_scanner::receipts::ItemService;
using receipt_scanner::receipts::kDirections;
using receipt_scanner::receipts::kKinds;
using receipt_scanner::receipts::NotFoundError;
using receipt_scanner::receipts::ConflictError;
using receipt_scanner::receipts::ReceiptNotFoundError;
using receipt_scanner::receipts::ReceiptService;
using receipt_scanner::receipts::CategoryService;
using receipt_scanner::receipts::PaymentMethodService;
using receipt_scanner::receipts::TagService;
using receipt_scanner::receipts::ValidationError;
using receipt_scanner::storage::ImageNormalizer;
using receipt_scanner::storage::S3ImageStore;

namespace {

// One process-lifetime client, same destructor-deadlock rationale as the other test files.
const drogon::orm::DbClientPtr &testDb() {
  static drogon::orm::DbClientPtr client = drogon::orm::DbClient::newPgClient(
      "host=" + envOr("DB_HOST", "localhost") + " port=" + envOr("DB_PORT", "5432") +
          " dbname=" + envOr("DB_NAME", "receipt_scanner") + " user=" + envOr("DB_USER", "receipt_scanner") +
          " password=" + envOr("DB_PASSWORD", ""),
      /*connSize=*/1);
  return client;
}

// ReceiptService needs an ImageStore + normalizer, but nothing in this file touches images, so
// the store is never actually called.
trantor::EventLoop &testLoop() {
  static trantor::EventLoopThread loopThread;
  static bool started = [] {
    loopThread.run();
    return true;
  }();
  (void)started;
  return *loopThread.getLoop();
}

ReceiptService receiptService() {
  static S3ImageStore::Config config{
      .endpoint = envOr("S3_ENDPOINT", "http://localhost:9000"),
      .region = envOr("S3_REGION", "us-east-1"),
      .bucket = envOr("S3_BUCKET", "receipts") + "-test",
      .accessKey = envOr("S3_ACCESS_KEY", "receipt_scanner"),
      .secretKey = envOr("S3_SECRET_KEY", "dev_only_change_me"),
      .pathStyle = envOr("S3_PATH_STYLE", "true") == "true",
  };
  static S3ImageStore store(config, &testLoop());
  static ImageNormalizer normalizer;
  return ReceiptService(testDb(), store, normalizer);
}

nlohmann::json body(const char *text) { return nlohmann::json::parse(text); }

drogon::Task<> wipe() { co_await testDb()->execSqlCoro("TRUNCATE users, receipts RESTART IDENTITY CASCADE"); }

// Through real registration, so each user gets the seeded categories + "cash" (spec Auth).
drogon::Task<int64_t> registerUser(const std::string &email) {
  receipt_scanner::auth::AuthService auth(testDb());
  auto user = co_await auth.registerUser(email, "a-fine-password");
  co_return user.userId;
}

drogon::Task<int64_t> idOf(const std::string &table, int64_t userId, const std::string &name) {
  auto result = co_await testDb()->execSqlCoro("SELECT id FROM " + table + " WHERE user_id = $1 AND name = $2",
                                               userId, name);
  co_return result[0]["id"].as<int64_t>();
}

drogon::Task<int64_t> scalar(const std::string &sql, int64_t arg) {
  auto result = co_await testDb()->execSqlCoro(sql, arg);
  co_return result[0][0].as<int64_t>();
}

// True iff awaiting `task` throws E. Lets a test assert on rejection without a co_await inside
// a Catch2 macro.
template <typename E, typename T> drogon::Task<bool> throws(drogon::Task<T> task) {
  bool threw = false;
  try {
    co_await task;
  } catch (const E &) {
    threw = true;
  }
  co_return threw;
}

// The quoted values in a CHECK constraint's definition, e.g. receipts_kind_check's
// "CHECK ((kind = ANY (ARRAY['purchase'::text, ...])))" -> {"purchase", ...}.
drogon::Task<std::set<std::string>> checkConstraintValues(const std::string &constraint) {
  auto result = co_await testDb()->execSqlCoro(
      "SELECT pg_get_constraintdef(oid) AS def FROM pg_constraint WHERE conname = $1", constraint);
  std::set<std::string> values;
  const auto def = result[0]["def"].as<std::string>();
  const std::regex quoted("'([a-z_]+)'::text");
  for (auto it = std::sregex_iterator(def.begin(), def.end(), quoted); it != std::sregex_iterator(); ++it) {
    values.insert((*it)[1].str());
  }
  co_return values;
}

// Whether Postgres accepts a bare receipt row with these values -- probes the CHECK constraints.
drogon::Task<bool> dbAccepts(int64_t userId, std::string kind, std::string direction, bool taxDeductible) {
  bool accepted = true;
  try {
    co_await testDb()->execSqlCoro(
        "INSERT INTO receipts (user_id, kind, direction, tax_deductible, source) VALUES ($1, $2, $3, $4, 'manual')",
        userId, kind, direction, taxDeductible);
  } catch (const std::exception &) {
    accepted = false;
  }
  co_return accepted;
}

} // namespace

// Kinds.h / ReceiptStates.h are the C++ source of truth for these domains, but migration 001's CHECK constraints
// are an independent copy -- this fails if they drift apart.
TEST_CASE("kinds: Kinds.h and ReceiptStates.h agree with the DB's CHECK constraints", "[ledger]") {
  drogon::sync_wait([&]() -> drogon::Task<> {
    co_await wipe();
    auto userId = co_await registerUser("kinds-table@example.com");

    std::set<std::string> cppKinds;
    for (const auto &rules : kKinds) {
      cppKinds.insert(std::string(rules.name));
    }
    std::set<std::string> cppDirections(kDirections.begin(), kDirections.end());
    auto dbKinds = co_await checkConstraintValues("receipts_kind_check");
    auto dbDirections = co_await checkConstraintValues("receipts_direction_check");
    CHECK(dbKinds == cppKinds);
    CHECK(dbDirections == cppDirections);

    namespace states = receipt_scanner::receipts;
    std::set<std::string> cppScanStates(states::scan_state::kAll.begin(), states::scan_state::kAll.end());
    std::set<std::string> cppSources(states::source::kAll.begin(), states::source::kAll.end());
    auto dbScanStates = co_await checkConstraintValues("receipts_scan_state_check");
    auto dbSources = co_await checkConstraintValues("receipts_source_check");
    CHECK(dbScanStates == cppScanStates);
    CHECK(dbSources == cppSources);

    for (const auto &rules : kKinds) {
      for (auto direction : kDirections) {
        const bool expected = !rules.canonicalDirection || *rules.canonicalDirection == direction;
        auto accepted = co_await dbAccepts(userId, std::string(rules.name), std::string(direction), false);
        INFO(rules.name << " / " << direction);
        CHECK(accepted == expected);
      }
      const std::string anyValidDirection(rules.canonicalDirection.value_or(kDirections[0]));
      auto deductibleAccepted = co_await dbAccepts(userId, std::string(rules.name), anyValidDirection, true);
      INFO(rules.name << " tax_deductible");
      CHECK(deductibleAccepted == rules.allowsTaxDeductible);
    }
  }());
}

TEST_CASE("manual: cash coffee is born reviewed with one implicit categorized item", "[ledger]") {
  auto service = receiptService();
  ItemService items(testDb());
  drogon::sync_wait([&]() -> drogon::Task<> {
    co_await wipe();
    auto userId = co_await registerUser("coffee@example.com");
    auto cash = co_await idOf("payment_methods", userId, "cash");
    auto dining = co_await idOf("categories", userId, "dining/takeaway");

    auto coffee = body(R"({"merchant": "Cafe Uno", "total_cents": 450})");
    coffee["payment_method_id"] = cash;
    coffee["category_id"] = dining;
    auto receiptId = co_await service.createManual(userId, coffee);

    auto receipt = co_await service.getReceipt(userId, receiptId);
    REQUIRE(receipt.has_value());
    CHECK(receipt->kind == "purchase");
    CHECK(receipt->direction == "outflow");
    CHECK(receipt->source == "manual");
    CHECK(receipt->scanState == "idle");
    CHECK(receipt->paymentMethodId == cash);
    CHECK(receipt->reviewedAt.has_value());
    CHECK_FALSE(receipt->proposedAt.has_value());
    CHECK(receipt->isConfirmed);
    CHECK_FALSE(receipt->needsReview);

    auto today = co_await testDb()->execSqlCoro("SELECT ((now() AT TIME ZONE 'Australia/Sydney')::date)::text AS d");
    CHECK(receipt->purchaseDate == today[0]["d"].as<std::string>());

    auto list = co_await items.list(userId, receiptId);
    REQUIRE(list.size() == 1);
    CHECK(list[0].name == "Cafe Uno");
    CHECK(list[0].amountCents == 450);
    CHECK(list[0].categoryId == dining);

    auto spend = co_await scalar("SELECT COALESCE(SUM(signed_amount_cents), 0) FROM v_countable_spend_items "
                                 "WHERE user_id = $1",
                                 userId);
    CHECK(spend == 450);
  }());
}

TEST_CASE("manual: refund links to its purchase and nets against spend", "[ledger]") {
  auto service = receiptService();
  drogon::sync_wait([&]() -> drogon::Task<> {
    co_await wipe();
    auto userId = co_await registerUser("refund@example.com");
    auto clothing = co_await idOf("categories", userId, "clothing");

    auto jacket = body(R"({"merchant": "Uniqlo", "total_cents": 8000})");
    jacket["category_id"] = clothing;
    auto purchaseId = co_await service.createManual(userId, jacket);

    auto refund = body(R"({"kind": "refund", "merchant": "Uniqlo", "total_cents": 8000})");
    refund["category_id"] = clothing;
    refund["offsets_receipt_id"] = purchaseId;
    auto refundId = co_await service.createManual(userId, refund);

    auto receipt = co_await service.getReceipt(userId, refundId);
    CHECK(receipt->direction == "inflow");
    CHECK(receipt->offsetsReceiptId == purchaseId);

    auto spend = co_await scalar("SELECT COALESCE(SUM(signed_spend_cents), 0) FROM v_countable_spend WHERE user_id = $1",
                                 userId);
    CHECK(spend == 0);

    // A refund can only offset a purchase, and only the user's own.
    auto refundOfRefund = body(R"({"kind": "refund", "total_cents": 100})");
    refundOfRefund["offsets_receipt_id"] = refundId;
    auto rejected = co_await throws<ValidationError>(service.createManual(userId, refundOfRefund));
    CHECK(rejected);
  }());
}

TEST_CASE("manual: transfer needs an explicit direction and counts as neither spend nor income", "[ledger]") {
  auto service = receiptService();
  drogon::sync_wait([&]() -> drogon::Task<> {
    co_await wipe();
    auto userId = co_await registerUser("atm@example.com");

    auto noDirection = body(R"({"kind": "transfer", "total_cents": 20000})");
    auto rejected = co_await throws<ValidationError>(service.createManual(userId, noDirection));
    CHECK(rejected);

    auto atm = body(R"({"kind": "transfer", "direction": "outflow", "merchant": "ATM", "total_cents": 20000})");
    auto id = co_await service.createManual(userId, atm);
    auto receipt = co_await service.getReceipt(userId, id);
    CHECK(receipt->direction == "outflow");
    CHECK(receipt->isConfirmed);

    auto inSpend = co_await scalar("SELECT count(*) FROM v_countable_spend WHERE user_id = $1", userId);
    auto inIncome = co_await scalar("SELECT count(*) FROM v_income WHERE user_id = $1", userId);
    CHECK(inSpend == 0);
    CHECK(inIncome == 0);
  }());
}

TEST_CASE("manual: income is inflow with an income category, in income stats and never in spend", "[ledger]") {
  auto service = receiptService();
  drogon::sync_wait([&]() -> drogon::Task<> {
    co_await wipe();
    auto userId = co_await registerUser("salary@example.com");
    auto salary = co_await idOf("categories", userId, "salary");
    auto groceries = co_await idOf("categories", userId, "groceries");

    auto pay = body(R"({"kind": "income", "merchant": "Employer Pty Ltd", "total_cents": 350000})");
    pay["category_id"] = salary;
    auto id = co_await service.createManual(userId, pay);
    auto receipt = co_await service.getReceipt(userId, id);
    CHECK(receipt->direction == "inflow");

    auto spendItems = co_await scalar("SELECT count(*) FROM v_countable_spend_items WHERE user_id = $1", userId);
    auto incomeItems = co_await scalar(
        "SELECT COALESCE(SUM(amount_cents), 0) FROM v_income_items WHERE user_id = $1", userId);
    CHECK(spendItems == 0);
    CHECK(incomeItems == 350000);

    // Expense category on income, and income category on a purchase, are both rejected.
    auto wrongSide = body(R"({"kind": "income", "total_cents": 100})");
    wrongSide["category_id"] = groceries;
    auto rejectedIncome = co_await throws<ValidationError>(service.createManual(userId, wrongSide));
    auto salaryOnPurchase = body(R"({"total_cents": 100})");
    salaryOnPurchase["category_id"] = salary;
    auto rejectedPurchase = co_await throws<ValidationError>(service.createManual(userId, salaryOnPurchase));
    CHECK(rejectedIncome);
    CHECK(rejectedPurchase);

    auto count = co_await scalar("SELECT count(*) FROM receipts WHERE user_id = $1", userId);
    CHECK(count == 1);
  }());
}

TEST_CASE("manual: zero total (warranty swap) is allowed", "[ledger]") {
  auto service = receiptService();
  drogon::sync_wait([&]() -> drogon::Task<> {
    co_await wipe();
    auto userId = co_await registerUser("warranty@example.com");
    auto swap = body(R"({"merchant": "JB Hi-Fi", "total_cents": 0, "note": "warranty swap"})");
    auto id = co_await service.createManual(userId, swap);
    auto receipt = co_await service.getReceipt(userId, id);
    CHECK(receipt->totalCents == 0);
  }());
}

TEST_CASE("manual: invalid kind/direction/field combinations are rejected and create nothing", "[ledger]") {
  auto service = receiptService();
  drogon::sync_wait([&]() -> drogon::Task<> {
    co_await wipe();
    auto userId = co_await registerUser("invalid@example.com");

    const char *cases[] = {
        R"({"total_cents": 100, "direction": "inflow"})",                          // purchase must be outflow
        R"({"kind": "income", "direction": "outflow", "total_cents": 100})",       // income must be inflow
        R"({"kind": "refund", "total_cents": 100, "tax_deductible": true})",       // purchases only
        R"({"kind": "reimbursement", "total_cents": 100, "tax_deductible": true})", // purchases only
        R"({"kind": "bogus", "total_cents": 100})",
        R"({"total_cents": -5})",                                                  // magnitude, never signed
        R"({"total_cents": 1.5})",                                                 // integer cents only
        R"({"merchant": "no total"})",
        R"({"total_cents": 100, "totl": 1})",                                      // unknown field
        R"({"total_cents": 100, "purchase_date": "2026-02-30"})",
        R"({"total_cents": 100, "fx_rate_to_aud": 1.5})",                          // AUD needs no rate
        R"({"total_cents": 100, "offsets_receipt_id": 1})",                        // purchase can't offset
        R"({"total_cents": 100, "category_id": 1, "items": [{"amount_cents": 100}]})",
        R"([1, 2])",
    };
    for (const char *text : cases) {
      auto bad = body(text);
      auto rejected = co_await throws<ValidationError>(service.createManual(userId, bad));
      INFO(text);
      CHECK(rejected);
    }

    auto count = co_await scalar("SELECT count(*) FROM receipts WHERE user_id = $1", userId);
    CHECK(count == 0);
  }());
}

TEST_CASE("manual: explicit items replace the implicit item; fx rate fills the AUD equivalent", "[ledger]") {
  auto service = receiptService();
  ItemService items(testDb());
  drogon::sync_wait([&]() -> drogon::Task<> {
    co_await wipe();
    auto userId = co_await registerUser("items@example.com");
    auto groceries = co_await idOf("categories", userId, "groceries");
    auto household = co_await idOf("categories", userId, "household");

    auto basket = body(R"({"merchant": "Costco", "total_cents": 1000, "currency": "usd", "fx_rate_to_aud": 1.5,
                           "items": [{"name": "milk", "amount_cents": 400}, {"name": "bin bags", "amount_cents": 600}]})");
    basket["items"][0]["category_id"] = groceries;
    basket["items"][1]["category_id"] = household;
    auto id = co_await service.createManual(userId, basket);

    auto receipt = co_await service.getReceipt(userId, id);
    CHECK(receipt->currency == "USD");
    CHECK(receipt->audEquivalentCents == 1500);

    auto list = co_await items.list(userId, id);
    REQUIRE(list.size() == 2);
    CHECK(list[0].categoryId == groceries);
    CHECK(list[1].categoryId == household);
  }());
}

TEST_CASE("manual: another tenant's payment method, category, offset target or tag is rejected", "[ledger]") {
  auto service = receiptService();
  drogon::sync_wait([&]() -> drogon::Task<> {
    co_await wipe();
    auto userA = co_await registerUser("tenant-a@example.com");
    auto userB = co_await registerUser("tenant-b@example.com");
    auto cashA = co_await idOf("payment_methods", userA, "cash");
    auto groceriesA = co_await idOf("categories", userA, "groceries");
    auto purchaseA = co_await service.createManual(userA, body(R"({"total_cents": 100})"));
    auto tag = body(R"({"name": "japan-trip"})");
    TagService tags(testDb());
    auto tagA = co_await tags.create(userA, tag);

    auto withPayment = body(R"({"total_cents": 100})");
    withPayment["payment_method_id"] = cashA;
    auto withCategory = body(R"({"total_cents": 100})");
    withCategory["category_id"] = groceriesA;
    auto withOffset = body(R"({"kind": "refund", "total_cents": 100})");
    withOffset["offsets_receipt_id"] = purchaseA;
    auto withTag = body(R"({"total_cents": 100, "items": [{"amount_cents": 100}]})");
    withTag["items"][0]["tag_ids"] = nlohmann::json::array();
    withTag["items"][0]["tag_ids"].push_back(tagA.id);

    auto r1 = co_await throws<ValidationError>(service.createManual(userB, withPayment));
    auto r2 = co_await throws<ValidationError>(service.createManual(userB, withCategory));
    auto r3 = co_await throws<ValidationError>(service.createManual(userB, withOffset));
    auto r4 = co_await throws<ValidationError>(service.createManual(userB, withTag));
    CHECK(r1);
    CHECK(r2);
    CHECK(r3);
    CHECK(r4);

    auto countB = co_await scalar("SELECT count(*) FROM receipts WHERE user_id = $1", userB);
    CHECK(countB == 0);
  }());
}

TEST_CASE("patch: editing keeps a reviewed receipt reviewed; reviewed=true confirms a pending one", "[ledger]") {
  auto service = receiptService();
  drogon::sync_wait([&]() -> drogon::Task<> {
    co_await wipe();
    auto userId = co_await registerUser("patch@example.com");

    auto id = co_await service.createManual(userId, body(R"({"merchant": "Cafe", "total_cents": 450})"));
    auto before = co_await service.getReceipt(userId, id);
    auto edit = body(R"({"merchant": "Cafe Uno", "note": "with a friend", "total_cents": 500})");
    auto after = co_await service.updateReceipt(userId, id, edit);
    CHECK(after.merchant == "Cafe Uno");
    CHECK(after.totalCents == 500);
    CHECK(after.reviewedAt == before->reviewedAt);
    CHECK(after.isConfirmed);

    // A scanned receipt awaiting review (proposed_at set, reviewed_at NULL).
    auto pending = co_await testDb()->execSqlCoro(
        "INSERT INTO receipts (user_id, kind, direction, source, scan_state, total_cents, proposed_at) "
        "VALUES ($1, 'purchase', 'outflow', 'scanned', 'idle', 1200, now() - interval '1 minute') RETURNING id",
        userId);
    auto pendingId = pending[0]["id"].as<int64_t>();
    auto beforeReview = co_await service.getReceipt(userId, pendingId);
    CHECK(beforeReview->needsReview);

    auto review = body(R"({"reviewed": true})");
    auto reviewed = co_await service.updateReceipt(userId, pendingId, review);
    CHECK_FALSE(reviewed.needsReview);
    CHECK(reviewed.isConfirmed);

    auto unreview = body(R"({"reviewed": false})");
    auto rejected = co_await throws<ValidationError>(service.updateReceipt(userId, pendingId, unreview));
    CHECK(rejected);
  }());
}

TEST_CASE("patch: kind changes re-derive direction and enforce the kind rules", "[ledger]") {
  auto service = receiptService();
  drogon::sync_wait([&]() -> drogon::Task<> {
    co_await wipe();
    auto userId = co_await registerUser("kinds@example.com");
    auto groceries = co_await idOf("categories", userId, "groceries");

    auto shop = body(R"({"total_cents": 900, "tax_deductible": true})");
    shop["category_id"] = groceries;
    auto id = co_await service.createManual(userId, shop);

    // tax_deductible (purchases only) must be cleared in the same edit.
    auto toRefundKeepingFlag = body(R"({"kind": "refund"})");
    auto keptFlag = co_await throws<ValidationError>(service.updateReceipt(userId, id, toRefundKeepingFlag));
    CHECK(keptFlag);
    auto toRefund = body(R"({"kind": "refund", "tax_deductible": false})");
    auto refund = co_await service.updateReceipt(userId, id, toRefund);
    CHECK(refund.direction == "inflow");

    auto toTransfer = body(R"({"kind": "transfer"})");
    auto transfer = co_await service.updateReceipt(userId, id, toTransfer);
    CHECK(transfer.direction == "inflow"); // transfer keeps whatever direction it had
    auto flip = body(R"({"direction": "outflow"})");
    auto flipped = co_await service.updateReceipt(userId, id, flip);
    CHECK(flipped.direction == "outflow");

    // Its implicit item is in an expense category, so it can't become income.
    auto toIncome = body(R"({"kind": "income"})");
    auto rejectedIncome = co_await throws<ValidationError>(service.updateReceipt(userId, id, toIncome));
    CHECK(rejectedIncome);

    auto purchaseInflow = body(R"({"kind": "purchase", "direction": "inflow"})");
    auto rejectedDirection = co_await throws<ValidationError>(service.updateReceipt(userId, id, purchaseInflow));
    CHECK(rejectedDirection);
  }());
}

TEST_CASE("patch: another tenant's receipt is not found", "[ledger]") {
  auto service = receiptService();
  drogon::sync_wait([&]() -> drogon::Task<> {
    co_await wipe();
    auto userA = co_await registerUser("patch-a@example.com");
    auto userB = co_await registerUser("patch-b@example.com");
    auto id = co_await service.createManual(userA, body(R"({"total_cents": 100})"));

    auto edit = body(R"({"note": "mine now"})");
    auto rejected = co_await throws<ReceiptNotFoundError>(service.updateReceipt(userB, id, edit));
    CHECK(rejected);
    auto unchanged = co_await service.getReceipt(userA, id);
    CHECK_FALSE(unchanged->note.has_value());
  }());
}

TEST_CASE("items: CRUD with tag assignment; informational items stay out of income sums", "[ledger]") {
  auto service = receiptService();
  ItemService items(testDb());
  TagService tags(testDb());
  drogon::sync_wait([&]() -> drogon::Task<> {
    co_await wipe();
    auto userId = co_await registerUser("payslip@example.com");
    auto salary = co_await idOf("categories", userId, "salary");
    auto workTag = body(R"({"name": "work"})");
    auto tag = co_await tags.create(userId, workTag);

    auto pay = body(R"({"kind": "income", "merchant": "Employer", "total_cents": 300000})");
    pay["category_id"] = salary;
    auto id = co_await service.createManual(userId, pay);

    auto gross = body(R"({"name": "gross pay", "amount_cents": 400000, "informational": true})");
    gross["tag_ids"] = nlohmann::json::array();
    gross["tag_ids"].push_back(tag.id);
    auto grossItem = co_await items.create(userId, id, gross);
    CHECK(grossItem.informational);
    CHECK(grossItem.tagIds == std::vector<int64_t>{tag.id});

    auto income = co_await scalar("SELECT COALESCE(SUM(amount_cents), 0) FROM v_income_items WHERE user_id = $1",
                                  userId);
    CHECK(income == 300000);

    auto rename = body(R"({"name": "gross", "tag_ids": []})");
    auto renamed = co_await items.update(userId, id, grossItem.id, rename);
    CHECK(renamed.name == "gross");
    CHECK(renamed.tagIds.empty());
    CHECK(renamed.amountCents == 400000); // untouched fields survive a partial update

    co_await items.remove(userId, id, grossItem.id);
    auto list = co_await items.list(userId, id);
    CHECK(list.size() == 1);

    auto gone = co_await throws<NotFoundError>(items.remove(userId, id, grossItem.id));
    CHECK(gone);
  }());
}

TEST_CASE("items: isolated between tenants and between receipts", "[ledger]") {
  auto service = receiptService();
  ItemService items(testDb());
  drogon::sync_wait([&]() -> drogon::Task<> {
    co_await wipe();
    auto userA = co_await registerUser("items-a@example.com");
    auto userB = co_await registerUser("items-b@example.com");
    auto receiptA = co_await service.createManual(userA, body(R"({"total_cents": 100})"));
    auto otherReceiptA = co_await service.createManual(userA, body(R"({"total_cents": 200})"));
    auto listA = co_await items.list(userA, receiptA);
    auto itemA = listA[0].id;

    auto listedByB = co_await throws<ReceiptNotFoundError>(items.list(userB, receiptA));
    auto edit = body(R"({"amount_cents": 1})");
    auto editedByB = co_await throws<ReceiptNotFoundError>(items.update(userB, receiptA, itemA, edit));
    auto deletedByB = co_await throws<ReceiptNotFoundError>(items.remove(userB, receiptA, itemA));
    // Right owner, wrong receipt: the item id doesn't belong to otherReceiptA.
    auto wrongReceipt = co_await throws<NotFoundError>(items.update(userA, otherReceiptA, itemA, edit));
    CHECK(listedByB);
    CHECK(editedByB);
    CHECK(deletedByB);
    CHECK(wrongReceipt);

    auto amount = co_await scalar("SELECT amount_cents FROM items WHERE id = $1", itemA);
    CHECK(amount == 100);
  }());
}

TEST_CASE("reference data: CRUD, case-insensitive duplicate names, tenant isolation", "[ledger]") {
  CategoryService categories(testDb());
  TagService tags(testDb());
  PaymentMethodService paymentMethods(testDb());
  drogon::sync_wait([&]() -> drogon::Task<> {
    co_await wipe();
    auto userA = co_await registerUser("ref-a@example.com");
    auto userB = co_await registerUser("ref-b@example.com");

    auto seeded = co_await categories.list(userA);
    CHECK(seeded.size() == 22); // 18 expense + 4 income (spec Category seed list)

    auto pets = body(R"({"name": "Pets"})");
    auto created = co_await categories.create(userA, pets);
    CHECK_FALSE(created.isIncome);

    auto dupe = body(R"({"name": "pets"})");
    auto duplicate = co_await throws<ConflictError>(categories.create(userA, dupe));
    CHECK(duplicate);
    // Uniqueness is per (user, expense/income side): the other side and other users are free.
    auto incomeOther = body(R"({"name": "other", "is_income": true})");
    auto otherSide = co_await categories.create(userA, incomeOther);
    CHECK(otherSide.isIncome);
    auto petsForB = co_await categories.create(userB, pets);
    CHECK(petsForB.id != created.id);

    auto rename = body(R"({"name": "Pet care"})");
    auto renamed = co_await categories.update(userA, created.id, rename);
    CHECK(renamed.name == "Pet care");

    auto renamedByB = co_await throws<NotFoundError>(categories.update(userB, created.id, rename));
    auto deletedByB = co_await throws<NotFoundError>(categories.remove(userB, created.id));
    CHECK(renamedByB);
    CHECK(deletedByB);

    auto card = body(R"({"name": "Opal", "stored_value": true})");
    auto opal = co_await paymentMethods.create(userA, card);
    CHECK(opal.storedValue);
    auto badKey = body(R"({"name": "x", "is_income": true})");
    auto rejectedKey = co_await throws<ValidationError>(tags.create(userA, badKey));
    CHECK(rejectedKey);
  }());
}

TEST_CASE("reference data: deleting a category uncategorizes its items; is_income can't flip while in use", "[ledger]") {
  auto service = receiptService();
  ItemService items(testDb());
  CategoryService categories(testDb());
  drogon::sync_wait([&]() -> drogon::Task<> {
    co_await wipe();
    auto userId = co_await registerUser("ref-delete@example.com");
    auto groceries = co_await idOf("categories", userId, "groceries");
    auto shop = body(R"({"total_cents": 900})");
    shop["category_id"] = groceries;
    auto id = co_await service.createManual(userId, shop);

    auto toIncome = body(R"({"is_income": true})");
    auto flipRejected =
        co_await throws<ValidationError>(categories.update(userId, groceries, toIncome));
    CHECK(flipRejected);

    co_await categories.remove(userId, groceries);
    auto list = co_await items.list(userId, id);
    REQUIRE(list.size() == 1);
    CHECK_FALSE(list[0].categoryId.has_value());
  }());
}

TEST_CASE("reference data: toggling stored_value moves past purchases in and out of spend, live", "[ledger]") {
  auto service = receiptService();
  PaymentMethodService paymentMethods(testDb());
  drogon::sync_wait([&]() -> drogon::Task<> {
    co_await wipe();
    auto userId = co_await registerUser("opal@example.com");
    auto card = body(R"({"name": "Gift card"})");
    auto giftCard = co_await paymentMethods.create(userId, card);

    auto purchase = body(R"({"total_cents": 2500})");
    purchase["payment_method_id"] = giftCard.id;
    co_await service.createManual(userId, purchase);

    auto before = co_await scalar("SELECT count(*) FROM v_countable_spend WHERE user_id = $1", userId);
    auto flag = body(R"({"stored_value": true})");
    co_await paymentMethods.update(userId, giftCard.id, flag);
    auto after = co_await scalar("SELECT count(*) FROM v_countable_spend WHERE user_id = $1", userId);
    CHECK(before == 1);
    CHECK(after == 0);
  }());
}

TEST_CASE("reference data: tag rename and payment-method partial update", "[ledger]") {
  TagService tags(testDb());
  PaymentMethodService paymentMethods(testDb());
  drogon::sync_wait([&]() -> drogon::Task<> {
    co_await wipe();
    auto userA = co_await registerUser("ref-update-a@example.com");
    auto userB = co_await registerUser("ref-update-b@example.com");

    auto work = body(R"({"name": "work"})");
    auto travel = body(R"({"name": "travel"})");
    auto workTag = co_await tags.create(userA, work);
    co_await tags.create(userA, travel);
    auto rename = body(R"({"name": "work-reimbursable"})");
    auto renamed = co_await tags.update(userA, workTag.id, rename);
    CHECK(renamed.name == "work-reimbursable");
    auto clashName = body(R"({"name": "TRAVEL"})");
    auto clash = co_await throws<ConflictError>(tags.update(userA, workTag.id, clashName));
    auto renamedByB = co_await throws<NotFoundError>(tags.update(userB, workTag.id, rename));
    CHECK(clash);
    CHECK(renamedByB);

    auto card = body(R"({"name": "Opal", "stored_value": true})");
    auto opal = co_await paymentMethods.create(userA, card);
    auto nameOnly = body(R"({"name": "Opal card"})");
    auto updated = co_await paymentMethods.update(userA, opal.id, nameOnly);
    CHECK(updated.name == "Opal card");
    CHECK(updated.storedValue); // absent field kept
    auto updatedByB = co_await throws<NotFoundError>(paymentMethods.update(userB, opal.id, nameOnly));
    CHECK(updatedByB);
  }());
}
