#include "config/Env.h"
#include "receipts/ReceiptService.h"
#include "storage/S3ImageStore.h"
#include "storage/Sigv4Signer.h"

#include <catch2/catch_test_macros.hpp>
#include <drogon/HttpClient.h>
#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>
#include <trantor/net/EventLoopThread.h>

#include <fstream>
#include <sstream>

using receipt_scanner::config::envOr;
using receipt_scanner::receipts::ImageConflict;
using receipt_scanner::receipts::ReceiptNotFoundError;
using receipt_scanner::receipts::ReceiptService;
using receipt_scanner::receipts::TooManyImagesError;
using receipt_scanner::storage::ImageNormalizer;
using receipt_scanner::storage::S3ImageStore;

namespace {

// Same process-lifetime-resource rationale as every other integration test in this suite
// (db_test.cpp, image_store_test.cpp): fresh DbClient/EventLoopThread per TEST_CASE risks
// the destructor-deadlock pattern found in Step 4/6. One of each, shared across this file.
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

S3ImageStore &testImageStore() {
  static S3ImageStore::Config config{
      .endpoint = envOr("S3_ENDPOINT", "http://localhost:9000"),
      .region = envOr("S3_REGION", "us-east-1"),
      .bucket = envOr("S3_BUCKET", "receipts") + "-test",
      .accessKey = envOr("S3_ACCESS_KEY", "receipt_scanner"),
      .secretKey = envOr("S3_SECRET_KEY", "dev_only_change_me"),
      .pathStyle = envOr("S3_PATH_STYLE", "true") == "true",
  };
  static S3ImageStore store(config, &testLoop());
  return store;
}

const ImageNormalizer &testNormalizer() {
  static ImageNormalizer normalizer;
  return normalizer;
}

ReceiptService makeService() {
  return ReceiptService(testDb(), testImageStore(), testNormalizer());
}

std::string readFixture(const std::string &name) {
  std::ifstream f(std::string(RECEIPT_SCANNER_SOURCE_DIR) + "/tests/fixtures/" + name, std::ios::binary);
  REQUIRE(f.is_open());
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

// A bare `std::vector<std::string>{...}` built inline as a co_await call argument ICEs
// GCC 12/13 (internal compiler error in build_special_member_call -- the initializer_list's
// compiler-generated backing array doesn't survive the coroutine-temporary analysis).
// Routing it through an ordinary function call sidesteps the list-init path entirely. Only
// reproduces under GCC; Clang (this Mac's local builds) compiles the bare braced form fine.
std::vector<std::string> oneImage(std::string fixtureBytes) { return {std::move(fixtureBytes)}; }

drogon::Task<> ensureBucketExists() {
  // Mirrors tests/image_store_test.cpp's setup -- MinIO needs the bucket to exist before
  // anything can be put into it; ReceiptService/ImageStore deliberately has no
  // bucket-management API (that's infra, provisioned once, not an app concern).
  using receipt_scanner::storage::Sigv4Signer;
  S3ImageStore::Config config{
      .endpoint = envOr("S3_ENDPOINT", "http://localhost:9000"),
      .region = envOr("S3_REGION", "us-east-1"),
      .bucket = envOr("S3_BUCKET", "receipts") + "-test",
      .accessKey = envOr("S3_ACCESS_KEY", "receipt_scanner"),
      .secretKey = envOr("S3_SECRET_KEY", "dev_only_change_me"),
      .pathStyle = envOr("S3_PATH_STYLE", "true") == "true",
  };
  Sigv4Signer signer(config.accessKey, config.secretKey, config.region, "s3");
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

drogon::Task<> wipeReceiptTables() {
  co_await testDb()->execSqlCoro("TRUNCATE users, receipts RESTART IDENTITY CASCADE");
}

drogon::Task<int64_t> makeUser(const std::string &email) {
  auto result =
      co_await testDb()->execSqlCoro("INSERT INTO users (email, password_hash) VALUES ($1, 'x') RETURNING id", email);
  co_return result[0]["id"].as<int64_t>();
}

} // namespace

TEST_CASE("upload: creates a receipt with one image", "[receipt]") {
  auto service = makeService();
  drogon::sync_wait([&]() -> drogon::Task<> {
    co_await ensureBucketExists();
    co_await wipeReceiptTables();
    auto userId = co_await makeUser("upload-test@example.com");

    auto result = co_await service.uploadReceipt(userId, oneImage(readFixture("tiny.png")), false);
    REQUIRE(std::holds_alternative<int64_t>(result));
    auto receiptId = std::get<int64_t>(result);

    auto receipt = co_await service.getReceipt(userId, receiptId);
    REQUIRE(receipt.has_value());
    REQUIRE(receipt->kind == "purchase");
    REQUIRE(receipt->direction == "outflow");
    REQUIRE(receipt->scanState == "uploaded");
    REQUIRE(receipt->source == "scanned");

    auto images = co_await service.listImages(userId, receiptId);
    REQUIRE(images.size() == 1);
  }());
}

TEST_CASE("upload: duplicate image hash is rejected, nothing created", "[receipt]") {
  auto service = makeService();
  drogon::sync_wait([&]() -> drogon::Task<> {
    co_await ensureBucketExists();
    co_await wipeReceiptTables();
    auto userId = co_await makeUser("dup-test@example.com");

    auto first = co_await service.uploadReceipt(userId, oneImage(readFixture("tiny.png")), false);
    REQUIRE(std::holds_alternative<int64_t>(first));
    auto firstReceiptId = std::get<int64_t>(first);

    auto second = co_await service.uploadReceipt(userId, oneImage(readFixture("tiny.png")), false);
    REQUIRE(std::holds_alternative<ImageConflict>(second));
    REQUIRE(std::get<ImageConflict>(second).receiptId == firstReceiptId);

    // Nothing was created for the rejected second upload.
    auto count = co_await testDb()->execSqlCoro("SELECT count(*) AS n FROM receipts WHERE user_id = $1", userId);
    REQUIRE(count[0]["n"].as<int64_t>() == 1);
  }());
}

TEST_CASE("upload: force=true bypasses the dedup check", "[receipt]") {
  auto service = makeService();
  drogon::sync_wait([&]() -> drogon::Task<> {
    co_await ensureBucketExists();
    co_await wipeReceiptTables();
    auto userId = co_await makeUser("force-test@example.com");

    co_await service.uploadReceipt(userId, oneImage(readFixture("tiny.png")), false);
    auto second = co_await service.uploadReceipt(userId, oneImage(readFixture("tiny.png")), true);
    REQUIRE(std::holds_alternative<int64_t>(second));

    auto count = co_await testDb()->execSqlCoro("SELECT count(*) AS n FROM receipts WHERE user_id = $1", userId);
    REQUIRE(count[0]["n"].as<int64_t>() == 2);
  }());
}

TEST_CASE("addImages: adds a second, distinct image to an existing receipt", "[receipt]") {
  auto service = makeService();
  drogon::sync_wait([&]() -> drogon::Task<> {
    co_await ensureBucketExists();
    co_await wipeReceiptTables();
    auto userId = co_await makeUser("add-test@example.com");

    auto created = co_await service.uploadReceipt(userId, oneImage(readFixture("tiny.png")), false);
    auto receiptId = std::get<int64_t>(created);

    auto added = co_await service.addImages(userId, receiptId, oneImage(readFixture("small.webp")), false);
    REQUIRE(std::holds_alternative<std::monostate>(added));

    auto images = co_await service.listImages(userId, receiptId);
    REQUIRE(images.size() == 2);
  }());
}

TEST_CASE("addImages: rejects a receipt it doesn't own", "[receipt]") {
  auto service = makeService();
  drogon::sync_wait([&]() -> drogon::Task<> {
    co_await ensureBucketExists();
    co_await wipeReceiptTables();
    auto ownerId = co_await makeUser("owner@example.com");
    auto attackerId = co_await makeUser("attacker@example.com");

    auto created = co_await service.uploadReceipt(ownerId, oneImage(readFixture("tiny.png")), false);
    auto receiptId = std::get<int64_t>(created);

    bool threw = false;
    try {
      co_await service.addImages(attackerId, receiptId, oneImage(readFixture("small.webp")), false);
    } catch (const ReceiptNotFoundError &) {
      threw = true;
    }
    REQUIRE(threw);
  }());
}

TEST_CASE("addImages: enforces the per-receipt image cap", "[receipt]") {
  auto service = makeService();
  drogon::sync_wait([&]() -> drogon::Task<> {
    co_await ensureBucketExists();
    co_await wipeReceiptTables();
    auto userId = co_await makeUser("cap-test@example.com");

    auto created = co_await service.uploadReceipt(userId, oneImage(readFixture("tiny.png")), false);
    auto receiptId = std::get<int64_t>(created);

    // Default cap is 8; this receipt already has 1. Force=true so the identical fixture
    // bytes across iterations don't trip the (unrelated) dedup check instead.
    for (int i = 0; i < 6; ++i) {
      co_await service.addImages(userId, receiptId, oneImage(readFixture("small.webp")), true);
    }
    // Now at 7. One more should succeed (reaching exactly 8)...
    co_await service.addImages(userId, receiptId, oneImage(readFixture("small.webp")), true);

    bool threw = false;
    try {
      // ...and this one should be rejected (would be 9, over the cap).
      co_await service.addImages(userId, receiptId, oneImage(readFixture("small.webp")), true);
    } catch (const TooManyImagesError &) {
      threw = true;
    }
    REQUIRE(threw);

    auto images = co_await service.listImages(userId, receiptId);
    REQUIRE(images.size() == 8);
  }());
}

TEST_CASE("getReceipt/listImages: isolated between tenants", "[receipt]") {
  auto service = makeService();
  drogon::sync_wait([&]() -> drogon::Task<> {
    co_await ensureBucketExists();
    co_await wipeReceiptTables();
    auto userA = co_await makeUser("a@example.com");
    auto userB = co_await makeUser("b@example.com");

    auto created = co_await service.uploadReceipt(userA, oneImage(readFixture("tiny.png")), false);
    auto receiptId = std::get<int64_t>(created);

    REQUIRE_FALSE((co_await service.getReceipt(userB, receiptId)).has_value());

    bool threw = false;
    try {
      co_await service.listImages(userB, receiptId);
    } catch (const ReceiptNotFoundError &) {
      threw = true;
    }
    REQUIRE(threw);
  }());
}

TEST_CASE("deleteReceipt: removes the row and the stored image, rejects non-owners", "[receipt]") {
  auto service = makeService();
  drogon::sync_wait([&]() -> drogon::Task<> {
    co_await ensureBucketExists();
    co_await wipeReceiptTables();
    auto userA = co_await makeUser("delete-a@example.com");
    auto userB = co_await makeUser("delete-b@example.com");

    auto created = co_await service.uploadReceipt(userA, oneImage(readFixture("tiny.png")), false);
    auto receiptId = std::get<int64_t>(created);
    auto location = co_await service.findImage((co_await service.listImages(userA, receiptId))[0].id);
    REQUIRE(location.has_value());

    REQUIRE_FALSE(co_await service.deleteReceipt(userB, receiptId)); // not the owner
    REQUIRE(co_await service.deleteReceipt(userA, receiptId));
    REQUIRE_FALSE(co_await service.deleteReceipt(userA, receiptId)); // already gone

    REQUIRE_FALSE((co_await service.getReceipt(userA, receiptId)).has_value());

    bool threw = false;
    try {
      co_await testImageStore().get(location->imageKey);
    } catch (const std::exception &) {
      threw = true;
    }
    REQUIRE(threw);
  }());
}

TEST_CASE("upload: undecodable bytes throw, creating nothing", "[receipt]") {
  auto service = makeService();
  drogon::sync_wait([&]() -> drogon::Task<> {
    co_await ensureBucketExists();
    co_await wipeReceiptTables();
    auto userId = co_await makeUser("garbage-test@example.com");

    bool threw = false;
    try {
      co_await service.uploadReceipt(userId, oneImage("not an image, just plain bytes"), false);
    } catch (const std::exception &) {
      threw = true;
    }
    REQUIRE(threw);

    auto count = co_await testDb()->execSqlCoro("SELECT count(*) AS n FROM receipts WHERE user_id = $1", userId);
    REQUIRE(count[0]["n"].as<int64_t>() == 0);
  }());
}
