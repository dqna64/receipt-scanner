// The scan worker against real Postgres + MinIO, with FakeScanProvider standing in for
// the LLM. Same GCC-safety conventions as ledger_test.cpp: no co_await inside a Catch2 macro,
// request bodies parsed from raw strings into named locals.

#include "auth/AuthService.h"
#include "config/Env.h"
#include "receipts/ItemService.h"
#include "receipts/ReceiptService.h"
#include "receipts/ReferenceDataService.h"
#include "scan/FakeScanProvider.h"
#include "scan/MerchantMatcher.h"
#include "scan/ScanWorker.h"
#include "storage/S3ImageStore.h"
#include "storage/Sigv4Signer.h"

#include <catch2/catch_test_macros.hpp>
#include <drogon/HttpClient.h>
#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>
#include <nlohmann/json.hpp>
#include <trantor/net/EventLoopThread.h>

#include <atomic>
#include <fstream>
#include <mutex>
#include <sstream>

using receipt_scanner::config::envOr;
using receipt_scanner::receipts::ConflictError;
using receipt_scanner::receipts::ItemService;
using receipt_scanner::receipts::ReceiptNotFoundError;
using receipt_scanner::receipts::ReceiptService;
using receipt_scanner::receipts::TagService;
using receipt_scanner::scan::FakeScanProvider;
using receipt_scanner::scan::PgTrgmMerchantMatcher;
using receipt_scanner::scan::ScanContext;
using receipt_scanner::scan::ScanError;
using receipt_scanner::scan::ScannedItem;
using receipt_scanner::scan::ScanResult;
using receipt_scanner::scan::ScanWorker;
using receipt_scanner::scan::ScanWorkerConfig;
using receipt_scanner::storage::ImageNormalizer;
using receipt_scanner::storage::S3ImageStore;

namespace {

// Process-lifetime resources, same destructor-deadlock rationale as the other test files.
const drogon::orm::DbClientPtr &testDb() {
  static drogon::orm::DbClientPtr client = drogon::orm::DbClient::newPgClient(
      "host=" + envOr("DB_HOST", "localhost") + " port=" + envOr("DB_PORT", "5432") +
          " dbname=" + envOr("DB_NAME", "receipt_scanner") + " user=" + envOr("DB_USER", "receipt_scanner") +
          " password=" + envOr("DB_PASSWORD", ""),
      /*connSize=*/1);
  return client;
}

trantor::EventLoop &testLoop() {
  static trantor::EventLoopThread loopThread;
  static bool started = [] {
    loopThread.run();
    return true;
  }();
  (void)started;
  return *loopThread.getLoop();
}

S3ImageStore::Config testS3Config() {
  return S3ImageStore::Config{
      .endpoint = envOr("S3_ENDPOINT", "http://localhost:9000"),
      .region = envOr("S3_REGION", "us-east-1"),
      .bucket = envOr("S3_BUCKET", "receipts") + "-test",
      .accessKey = envOr("S3_ACCESS_KEY", "receipt_scanner"),
      .secretKey = envOr("S3_SECRET_KEY", "dev_only_change_me"),
      .pathStyle = envOr("S3_PATH_STYLE", "true") == "true",
  };
}

S3ImageStore &testImageStore() {
  static S3ImageStore store(testS3Config(), &testLoop());
  return store;
}

ReceiptService receiptService() {
  static ImageNormalizer normalizer;
  return ReceiptService(testDb(), testImageStore(), normalizer);
}

PgTrgmMerchantMatcher &testMatcher() {
  static PgTrgmMerchantMatcher matcher(testDb());
  return matcher;
}

// Records every notification instead of sending it.
class RecordingNotifier : public receipt_scanner::notify::Notifier {
public:
  drogon::Task<> notify(int64_t userId, std::string event, nlohmann::json payload) override {
    std::lock_guard lock(mutex_);
    events_.push_back({userId, std::move(event), std::move(payload)});
    co_return;
  }
  struct Event {
    int64_t userId;
    std::string event;
    nlohmann::json payload;
  };
  std::vector<Event> events() const {
    std::lock_guard lock(mutex_);
    return events_;
  }

private:
  mutable std::mutex mutex_;
  std::vector<Event> events_;
};

ScanWorker makeWorker(FakeScanProvider &provider, RecordingNotifier &notifier, ScanWorkerConfig config = {}) {
  return ScanWorker(testDb(), testImageStore(), provider, testMatcher(), notifier, config);
}

std::string readFixture(const std::string &name) {
  std::ifstream f(std::string(RECEIPT_SCANNER_SOURCE_DIR) + "/tests/fixtures/" + name, std::ios::binary);
  REQUIRE(f.is_open());
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

// Through a function call, never an inline braced list in a co_await argument (GCC 12/13 ICE,
// see receipt_test.cpp's oneImage).
std::vector<std::string> oneImage(std::string bytes) { return {std::move(bytes)}; }
std::vector<std::string> twoImages(std::string a, std::string b) { return {std::move(a), std::move(b)}; }

nlohmann::json body(const char *text) { return nlohmann::json::parse(text); }

// Same as receipt_test.cpp: the test bucket must exist before anything is put into it.
drogon::Task<> ensureBucketExists() {
  const auto config = testS3Config();
  receipt_scanner::storage::Sigv4Signer signer(config.accessKey, config.secretKey, config.region, "s3");
  auto client = drogon::HttpClient::newHttpClient(config.endpoint, &testLoop());
  auto schemeEnd = config.endpoint.find("://");
  auto host = schemeEnd == std::string::npos ? config.endpoint : config.endpoint.substr(schemeEnd + 3);
  auto uri = "/" + config.bucket;
  auto signed_ = signer.sign("PUT", uri, host, "");
  auto req = drogon::HttpRequest::newHttpRequest();
  req->setMethod(drogon::Put);
  req->setPath(uri);
  req->setPathEncode(false);
  req->addHeader("Host", host);
  req->addHeader("x-amz-date", signed_.amzDate);
  req->addHeader("x-amz-content-sha256", signed_.contentSha256);
  req->addHeader("Authorization", signed_.authorization);
  co_await client->sendRequestCoro(req);
}

drogon::Task<> wipe() { co_await testDb()->execSqlCoro("TRUNCATE users, receipts RESTART IDENTITY CASCADE"); }

drogon::Task<int64_t> registerUser(const std::string &email) {
  receipt_scanner::auth::AuthService auth(testDb());
  auto user = co_await auth.registerUser(email, "a-fine-password");
  co_return user.userId;
}

drogon::Task<int64_t> scalar(const std::string &sql, int64_t arg) {
  auto result = co_await testDb()->execSqlCoro(sql, arg);
  co_return result[0][0].as<int64_t>();
}

drogon::Task<std::string> text(const std::string &sql, int64_t arg) {
  auto result = co_await testDb()->execSqlCoro(sql, arg);
  co_return result[0][0].isNull() ? std::string("<null>") : result[0][0].as<std::string>();
}

// A result whose kind is the row's known kind in verify mode, purchase otherwise.
ScanResult result(const ScanContext &context, int64_t totalCents, const char *merchant, const char *date) {
  ScanResult r;
  r.kind = context.knownKind.value_or("purchase");
  r.totalCents = totalCents;
  r.merchant = merchant;
  r.purchaseDate = date;
  r.raw = nlohmann::json::object();
  r.raw["total_cents"] = totalCents;
  return r;
}

// A fixed result for every call; records each call's context.
FakeScanProvider::Handler returning(ScanResult fixed, std::vector<ScanContext> *contexts = nullptr) {
  return [fixed, contexts](const std::vector<std::string> &, const ScanContext &context) -> drogon::Task<ScanResult> {
    if (contexts) {
      contexts->push_back(context);
    }
    co_return fixed;
  };
}

} // namespace

TEST_CASE("worker: extraction scan sends the full image set and writes fields + mapped items", "[scan]") {
  auto service = receiptService();
  std::vector<ScanContext> contexts;
  ScanResult scanned;
  scanned.kind = "purchase";
  scanned.merchant = "Woolworths";
  scanned.storeNameRaw = "WOOLWORTHS 1234 SYDNEY";
  scanned.totalCents = 2350;
  scanned.purchaseDate = "2026-10-01";
  ScannedItem milk;
  milk.name = "milk";
  milk.amountCents = 350;
  milk.category = "Groceries"; // case differs from the seed: matched case-insensitively
  milk.tags.push_back("work");
  ScannedItem bread;
  bread.name = "bread";
  bread.amountCents = 2000;
  bread.category = "not-a-category";
  scanned.items.push_back(milk);
  scanned.items.push_back(bread);
  scanned.raw = nlohmann::json::object();
  scanned.raw["source"] = "test";
  FakeScanProvider provider(returning(scanned, &contexts));
  RecordingNotifier notifier;
  auto worker = makeWorker(provider, notifier);

  drogon::sync_wait([&]() -> drogon::Task<> {
    co_await ensureBucketExists();
    co_await wipe();
    auto userId = co_await registerUser("extract@example.com");
    TagService tags(testDb());
    auto workTag = body(R"({"name": "work"})");
    auto tag = co_await tags.create(userId, workTag);
    auto uploaded = co_await service.uploadReceipt(userId, twoImages(readFixture("tiny.png"), readFixture("small.webp")),
                                                   false);
    auto receiptId = std::get<int64_t>(uploaded);

    auto claimed = co_await worker.tick();
    CHECK(claimed == 1);
    CHECK(provider.imageCounts() == std::vector<size_t>{2});
    REQUIRE(contexts.size() == 1);
    CHECK_FALSE(contexts[0].knownKind.has_value()); // extraction: the provider classifies
    CHECK(contexts[0].incomeCategories.size() == 4);

    auto receipt = co_await service.getReceipt(userId, receiptId);
    CHECK(receipt->scanState == "idle");
    CHECK(receipt->needsReview);
    CHECK_FALSE(receipt->isConfirmed);
    CHECK(receipt->merchant == "Woolworths");
    CHECK(receipt->storeNameRaw == "WOOLWORTHS 1234 SYDNEY");
    CHECK(receipt->totalCents == 2350);
    CHECK(receipt->purchaseDate == "2026-10-01");
    auto raw = co_await text("SELECT raw_ocr_json->>'source' FROM receipts WHERE id = $1", receiptId);
    CHECK(raw == "test");

    ItemService items(testDb());
    auto list = co_await items.list(userId, receiptId);
    REQUIRE(list.size() == 2);
    auto groceries = co_await scalar("SELECT id FROM categories WHERE user_id = $1 AND name = 'groceries'", userId);
    CHECK(list[0].categoryId == groceries);
    CHECK(list[0].tagIds == std::vector<int64_t>{tag.id});
    CHECK_FALSE(list[1].categoryId.has_value());
  }());
}

TEST_CASE("worker: verify scan that matches keeps a reviewed receipt confirmed, filling only empty fields", "[scan]") {
  auto service = receiptService();
  std::vector<ScanContext> contexts;
  FakeScanProvider provider([&](const std::vector<std::string> &, const ScanContext &context) -> drogon::Task<ScanResult> {
    contexts.push_back(context);
    auto r = result(context, 2350, "WOOLWORTHS METRO", "2026-10-03"); // date within the 3-day tolerance
    r.storeNameRaw = "WOOLWORTHS METRO 1234";
    co_return r;
  });
  RecordingNotifier notifier;
  auto worker = makeWorker(provider, notifier);

  drogon::sync_wait([&]() -> drogon::Task<> {
    co_await ensureBucketExists();
    co_await wipe();
    auto userId = co_await registerUser("verify-match@example.com");
    auto manual = body(R"({"merchant": "Woolworths Metro", "total_cents": 2350, "purchase_date": "2026-10-01"})");
    auto receiptId = co_await service.createManual(userId, manual);
    co_await service.addImages(userId, receiptId, oneImage(readFixture("tiny.png")), false);

    co_await worker.tick();
    REQUIRE(contexts.size() == 1);
    CHECK(contexts[0].knownKind == "purchase");

    auto receipt = co_await service.getReceipt(userId, receiptId);
    CHECK(receipt->scanState == "idle");
    CHECK(receipt->isConfirmed);
    CHECK_FALSE(receipt->needsReview);
    CHECK(receipt->proposedAt.has_value());
    CHECK(receipt->merchant == "Woolworths Metro"); // recorded value kept
    CHECK(receipt->purchaseDate == "2026-10-01");
    CHECK(receipt->storeNameRaw == "WOOLWORTHS METRO 1234"); // was empty: filled
    auto itemCount = co_await scalar("SELECT count(*) FROM items WHERE receipt_id = $1", receiptId);
    CHECK(itemCount == 1); // the implicit item; scanned items only fill an itemless receipt
  }());
}

TEST_CASE("worker: verify scan that mismatches sends a reviewed receipt to review, overwriting nothing", "[scan]") {
  auto service = receiptService();
  FakeScanProvider provider([](const std::vector<std::string> &, const ScanContext &context) -> drogon::Task<ScanResult> {
    co_return result(context, 2400, "Woolworths Metro", "2026-10-01");
  });
  RecordingNotifier notifier;
  auto worker = makeWorker(provider, notifier);

  drogon::sync_wait([&]() -> drogon::Task<> {
    co_await ensureBucketExists();
    co_await wipe();
    auto userId = co_await registerUser("verify-mismatch@example.com");
    auto manual = body(R"({"merchant": "Woolworths Metro", "total_cents": 2350, "purchase_date": "2026-10-01"})");
    auto receiptId = co_await service.createManual(userId, manual);
    co_await service.addImages(userId, receiptId, oneImage(readFixture("tiny.png")), false);

    co_await worker.tick();
    auto receipt = co_await service.getReceipt(userId, receiptId);
    CHECK(receipt->needsReview);
    CHECK_FALSE(receipt->isConfirmed);
    CHECK(receipt->totalCents == 2350);
    auto rawTotal = co_await text("SELECT raw_ocr_json->>'total_cents' FROM receipts WHERE id = $1", receiptId);
    CHECK(rawTotal == "2400"); // the diff source for the review screen
  }());
}

TEST_CASE("worker: provider errors spend scan_attempts up to scan_failed, notify once, keep reviewed rows reviewed",
          "[scan]") {
  auto service = receiptService();
  FakeScanProvider provider([](const std::vector<std::string> &, const ScanContext &) -> drogon::Task<ScanResult> {
    throw ScanError("model returned garbage");
    co_return ScanResult{};
  });
  RecordingNotifier notifier;
  auto worker = makeWorker(provider, notifier);

  drogon::sync_wait([&]() -> drogon::Task<> {
    co_await ensureBucketExists();
    co_await wipe();
    auto userId = co_await registerUser("scan-errors@example.com");
    auto manual = body(R"({"merchant": "Cafe", "total_cents": 450})");
    auto receiptId = co_await service.createManual(userId, manual);
    auto before = co_await service.getReceipt(userId, receiptId);
    co_await service.addImages(userId, receiptId, oneImage(readFixture("tiny.png")), false);

    co_await worker.tick();
    auto afterOne = co_await service.getReceipt(userId, receiptId);
    auto attemptsAfterOne = co_await scalar("SELECT scan_attempts FROM receipts WHERE id = $1", receiptId);
    CHECK(afterOne->scanState == "uploaded");
    CHECK(attemptsAfterOne == 1);

    co_await worker.tick();
    co_await worker.tick();
    auto failed = co_await service.getReceipt(userId, receiptId);
    auto attempts = co_await scalar("SELECT scan_attempts FROM receipts WHERE id = $1", receiptId);
    auto reclaims = co_await scalar("SELECT reclaim_count FROM receipts WHERE id = $1", receiptId);
    CHECK(failed->scanState == "scan_failed");
    CHECK(attempts == 3);
    CHECK(reclaims == 0);
    CHECK(failed->isConfirmed); // terminal failure touches neither timestamp
    CHECK(failed->reviewedAt == before->reviewedAt);
    CHECK_FALSE(failed->proposedAt.has_value());

    auto extra = co_await worker.tick(); // scan_failed is never claimed again
    CHECK(extra == 0);
    auto events = notifier.events();
    REQUIRE(events.size() == 1);
    CHECK(events[0].event == "scan_failed");
    CHECK(events[0].userId == userId);
    CHECK(events[0].payload["receipt_id"] == receiptId);
  }());
}

TEST_CASE("worker: a receipt with no images fails at once, without calling the provider", "[scan]") {
  auto service = receiptService();
  FakeScanProvider provider;
  RecordingNotifier notifier;
  auto worker = makeWorker(provider, notifier);

  drogon::sync_wait([&]() -> drogon::Task<> {
    co_await ensureBucketExists();
    co_await wipe();
    auto userId = co_await registerUser("noimages@example.com");
    auto uploaded = co_await service.uploadReceipt(userId, oneImage(readFixture("tiny.png")), false);
    auto receiptId = std::get<int64_t>(uploaded);
    co_await testDb()->execSqlCoro("DELETE FROM receipt_images WHERE receipt_id = $1", receiptId);

    co_await worker.tick();
    auto state = co_await text("SELECT scan_state FROM receipts WHERE id = $1", receiptId);
    auto attempts = co_await scalar("SELECT scan_attempts FROM receipts WHERE id = $1", receiptId);
    CHECK(state == "scan_failed");
    CHECK(attempts == 1);
    CHECK(provider.imageCounts().empty());
    CHECK(notifier.events().size() == 1);
  }());
}

TEST_CASE("worker: a scan killed mid-flight is reclaimed without spending an attempt, then completes", "[scan]") {
  auto service = receiptService();
  FakeScanProvider provider;
  RecordingNotifier notifier;
  auto worker = makeWorker(provider, notifier);

  drogon::sync_wait([&]() -> drogon::Task<> {
    co_await ensureBucketExists();
    co_await wipe();
    auto userId = co_await registerUser("reclaim@example.com");
    auto uploaded = co_await service.uploadReceipt(userId, oneImage(readFixture("tiny.png")), false);
    auto receiptId = std::get<int64_t>(uploaded);
    // What a process dying mid-scan leaves behind: claimed 11 minutes ago, never finished.
    co_await testDb()->execSqlCoro(
        "UPDATE receipts SET scan_state = 'scanning', scan_started_at = now() - interval '11 minutes' WHERE id = $1",
        receiptId);

    co_await worker.tick(); // reclaim, then claim and scan in the same tick
    auto receipt = co_await service.getReceipt(userId, receiptId);
    auto attempts = co_await scalar("SELECT scan_attempts FROM receipts WHERE id = $1", receiptId);
    auto reclaims = co_await scalar("SELECT reclaim_count FROM receipts WHERE id = $1", receiptId);
    CHECK(receipt->scanState == "idle");
    CHECK(receipt->merchant == "Fake Merchant");
    CHECK(attempts == 0);
    CHECK(reclaims == 1);
    CHECK(notifier.events().empty());
  }());
}

TEST_CASE("worker: the reclaim backstop sends a scan that keeps hanging to scan_failed", "[scan]") {
  auto service = receiptService();
  FakeScanProvider provider;
  RecordingNotifier notifier;
  ScanWorkerConfig config;
  config.maxReclaims = 2;
  auto worker = makeWorker(provider, notifier, config);

  drogon::sync_wait([&]() -> drogon::Task<> {
    co_await ensureBucketExists();
    co_await wipe();
    auto userId = co_await registerUser("backstop@example.com");
    auto uploaded = co_await service.uploadReceipt(userId, oneImage(readFixture("tiny.png")), false);
    auto receiptId = std::get<int64_t>(uploaded);
    co_await testDb()->execSqlCoro("UPDATE receipts SET scan_state = 'scanning', reclaim_count = 1, "
                                   "scan_started_at = now() - interval '11 minutes' WHERE id = $1",
                                   receiptId);

    auto claimed = co_await worker.tick();
    auto receipt = co_await service.getReceipt(userId, receiptId);
    CHECK(claimed == 0);
    CHECK(receipt->scanState == "scan_failed");
    CHECK(notifier.events().size() == 1);
  }());
}

TEST_CASE("worker: a result for a claim that went stale mid-scan is discarded", "[scan]") {
  auto service = receiptService();
  int64_t receiptId = 0;
  // While the "LLM call" is in flight, another worker reclaims and re-claims the row.
  FakeScanProvider provider([&](const std::vector<std::string> &, const ScanContext &context) -> drogon::Task<ScanResult> {
    co_await testDb()->execSqlCoro("UPDATE receipts SET scan_started_at = clock_timestamp() WHERE id = $1",
                                   receiptId);
    co_return result(context, 999, "Stale Merchant", "2026-10-01");
  });
  RecordingNotifier notifier;
  auto worker = makeWorker(provider, notifier);

  drogon::sync_wait([&]() -> drogon::Task<> {
    co_await ensureBucketExists();
    co_await wipe();
    auto userId = co_await registerUser("stale@example.com");
    auto uploaded = co_await service.uploadReceipt(userId, oneImage(readFixture("tiny.png")), false);
    receiptId = std::get<int64_t>(uploaded);

    co_await worker.tick();
    auto receipt = co_await service.getReceipt(userId, receiptId);
    CHECK(receipt->scanState == "scanning"); // still the other claim's to finish
    CHECK_FALSE(receipt->merchant.has_value());
    CHECK_FALSE(receipt->proposedAt.has_value());
  }());
}

TEST_CASE("worker: a hung scan blocks neither its siblings nor later claims once reclaimed", "[scan]") {
  auto service = receiptService();
  std::atomic<bool> release = false;
  std::atomic<int> twoImageCalls = 0;
  // The first scan of the two-image receipt hangs (a provider call that never returns) until released.
  FakeScanProvider provider([&](const std::vector<std::string> &images, const ScanContext &context) -> drogon::Task<ScanResult> {
    if (images.size() == 2 && twoImageCalls++ == 0) {
      while (!release) {
        co_await drogon::sleepCoro(&testLoop(), 0.02);
      }
      co_return result(context, 999, "Stale Merchant", "2026-10-01");
    }
    co_return result(context, 500, "Fresh Merchant", "2026-10-01");
  });
  RecordingNotifier notifier;
  ScanWorkerConfig config;
  config.concurrency = 2;
  auto worker = makeWorker(provider, notifier, config);

  drogon::sync_wait([&]() -> drogon::Task<> {
    co_await ensureBucketExists();
    co_await wipe();
    auto userId = co_await registerUser("hang@example.com");
    auto hungUpload =
        co_await service.uploadReceipt(userId, twoImages(readFixture("tiny.png"), readFixture("small.webp")), true);
    auto hungId = std::get<int64_t>(hungUpload);
    std::vector<int64_t> ids;
    for (int i = 0; i < 3; ++i) {
      auto uploaded = co_await service.uploadReceipt(userId, oneImage(readFixture("tiny.png")), /*force=*/true);
      ids.push_back(std::get<int64_t>(uploaded));
    }

    // Claims the hung receipt and ids[0]; never completes while the scan hangs.
    std::atomic<bool> hungTickDone = false;
    drogon::async_run([&]() -> drogon::Task<> {
      co_await worker.tick();
      hungTickDone = true;
    });
    std::string siblingState;
    for (int i = 0; i < 250 && siblingState != "idle"; ++i) {
      co_await drogon::sleepCoro(&testLoop(), 0.02);
      siblingState = co_await text("SELECT scan_state FROM receipts WHERE id = $1", ids[0]);
    }
    CHECK(siblingState == "idle");
    CHECK_FALSE(hungTickDone);

    // The hung scan still holds one of the two slots.
    auto claimed = co_await worker.tick();
    CHECK(claimed == 1);
    auto waiting = co_await text("SELECT scan_state FROM receipts WHERE id = $1", ids[2]);
    CHECK(waiting == "uploaded");

    // The reclaim timeout passes: the hung scan's row is re-queued and its slot is free again.
    co_await testDb()->execSqlCoro(
        "UPDATE receipts SET scan_started_at = scan_started_at - interval '11 minutes' WHERE id = $1", hungId);
    auto afterReclaim = co_await worker.tick(); // re-claims the hung receipt (oldest) and ids[2]
    CHECK(afterReclaim == 2);
    auto newest = co_await text("SELECT scan_state FROM receipts WHERE id = $1", ids[2]);
    CHECK(newest == "idle");
    auto reclaims = co_await scalar("SELECT reclaim_count FROM receipts WHERE id = $1", hungId);
    CHECK(reclaims == 1);

    // The original hung call finally returns; its stale claim must not overwrite anything.
    release = true;
    for (int i = 0; i < 250 && !hungTickDone; ++i) {
      co_await drogon::sleepCoro(&testLoop(), 0.02);
    }
    CHECK(hungTickDone);
    auto merchant = co_await text("SELECT merchant FROM receipts WHERE id = $1", hungId);
    CHECK(merchant == "Fresh Merchant");
  }());
}

TEST_CASE("worker: claims at most `concurrency` receipts per tick, oldest first", "[scan]") {
  auto service = receiptService();
  FakeScanProvider provider;
  RecordingNotifier notifier;
  ScanWorkerConfig config;
  config.concurrency = 2;
  auto worker = makeWorker(provider, notifier, config);

  drogon::sync_wait([&]() -> drogon::Task<> {
    co_await ensureBucketExists();
    co_await wipe();
    auto userId = co_await registerUser("concurrency@example.com");
    std::vector<int64_t> ids;
    for (int i = 0; i < 3; ++i) {
      auto uploaded = co_await service.uploadReceipt(userId, oneImage(readFixture("tiny.png")), /*force=*/true);
      ids.push_back(std::get<int64_t>(uploaded));
    }

    auto claimed = co_await worker.tick();
    CHECK(claimed == 2);
    auto newest = co_await service.getReceipt(userId, ids[2]);
    CHECK(newest->scanState == "uploaded");
    auto oldest = co_await service.getReceipt(userId, ids[0]);
    CHECK(oldest->scanState == "idle");
  }());
}

TEST_CASE("worker: extraction reconciles fields the scanned kind constrains", "[scan]") {
  auto service = receiptService();
  FakeScanProvider provider([](const std::vector<std::string> &, const ScanContext &) -> drogon::Task<ScanResult> {
    ScanResult r;
    r.kind = "refund";
    r.totalCents = 500;
    r.raw = nlohmann::json::object();
    co_return r;
  });
  RecordingNotifier notifier;
  auto worker = makeWorker(provider, notifier);

  drogon::sync_wait([&]() -> drogon::Task<> {
    co_await ensureBucketExists();
    co_await wipe();
    auto userId = co_await registerUser("reconcile@example.com");
    auto uploaded = co_await service.uploadReceipt(userId, oneImage(readFixture("tiny.png")), false);
    auto receiptId = std::get<int64_t>(uploaded);
    auto deductible = body(R"({"tax_deductible": true})");
    co_await service.updateReceipt(userId, receiptId, deductible); // fine while it's a purchase

    co_await worker.tick();
    auto receipt = co_await service.getReceipt(userId, receiptId);
    CHECK(receipt->kind == "refund");
    CHECK(receipt->direction == "inflow");
    CHECK_FALSE(receipt->taxDeductible); // a refund can't be deductible (also a DB CHECK)
  }());
}

TEST_CASE("retry-scan: re-queues scan_failed with fresh budgets; rejects other states and tenants", "[scan]") {
  auto service = receiptService();
  drogon::sync_wait([&]() -> drogon::Task<> {
    co_await ensureBucketExists();
    co_await wipe();
    auto userA = co_await registerUser("retry-a@example.com");
    auto userB = co_await registerUser("retry-b@example.com");
    auto uploaded = co_await service.uploadReceipt(userA, oneImage(readFixture("tiny.png")), false);
    auto receiptId = std::get<int64_t>(uploaded);

    bool notFailedRejected = false;
    try {
      co_await service.retryScan(userA, receiptId);
    } catch (const ConflictError &) {
      notFailedRejected = true;
    }
    CHECK(notFailedRejected);

    co_await testDb()->execSqlCoro(
        "UPDATE receipts SET scan_state = 'scan_failed', scan_attempts = 3, reclaim_count = 4 WHERE id = $1", receiptId);
    bool otherTenantRejected = false;
    try {
      co_await service.retryScan(userB, receiptId);
    } catch (const ReceiptNotFoundError &) {
      otherTenantRejected = true;
    }
    CHECK(otherTenantRejected);

    auto retried = co_await service.retryScan(userA, receiptId);
    auto attempts = co_await scalar("SELECT scan_attempts FROM receipts WHERE id = $1", receiptId);
    auto reclaims = co_await scalar("SELECT reclaim_count FROM receipts WHERE id = $1", receiptId);
    CHECK(retried.scanState == "uploaded");
    CHECK(attempts == 0);
    CHECK(reclaims == 0);
  }());
}
