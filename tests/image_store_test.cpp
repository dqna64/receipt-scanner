#include "config/Env.h"
#include "storage/S3ImageStore.h"
#include "storage/Sigv4Signer.h"

#include <catch2/catch_test_macros.hpp>
#include <drogon/HttpClient.h>
#include <drogon/utils/coroutine.h>
#include <trantor/net/EventLoopThread.h>

#include <stdexcept>

using receipt_scanner::config::envOr;
using receipt_scanner::storage::S3ImageStore;
using receipt_scanner::storage::Sigv4Signer;

namespace {

// HttpClient needs an EventLoop actually pumping I/O -- there's no drogon::app() running in
// this test binary, so a shared, explicitly-started loop stands in for it (see the comment
// on S3ImageStore's constructor for what happens without one: every request hangs forever,
// nothing sent on the wire, no error). One loop shared across the whole test binary, same
// process-lifetime-resource rationale as tests/db_test.cpp's shared DbClient.
trantor::EventLoop &testLoop() {
  static trantor::EventLoopThread loopThread;
  static bool started = [] {
    loopThread.run();
    return true;
  }();
  (void)started;
  return *loopThread.getLoop();
}

S3ImageStore::Config testConfig() {
  return S3ImageStore::Config{
      .endpoint = envOr("S3_ENDPOINT", "http://localhost:9000"),
      .region = envOr("S3_REGION", "us-east-1"),
      .bucket = envOr("S3_BUCKET", "receipts") + "-test", // dedicated test bucket, never the real one
      .accessKey = envOr("S3_ACCESS_KEY", "receipt_scanner"),
      .secretKey = envOr("S3_SECRET_KEY", "dev_only_change_me"),
      .pathStyle = envOr("S3_PATH_STYLE", "true") == "true",
  };
}

// MinIO (like real S3) requires a bucket to exist before objects can be put into it.
// ImageStore deliberately has no bucket-management API (spec scope: Put/Get/Delete OBJECTS,
// not bucket lifecycle) -- this is test-only setup, idempotent (MinIO accepts a repeat
// "create" of a bucket it already owns).
drogon::Task<> ensureBucketExists(const S3ImageStore::Config &config) {
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

  auto resp = co_await client->sendRequestCoro(req);
  // 200 = created; 409 = already exists (BucketAlreadyOwnedByYou on MinIO) -- both fine.
  if (resp->statusCode() != drogon::k200OK && resp->statusCode() != drogon::k409Conflict) {
    throw std::runtime_error("failed to create test bucket: HTTP " + std::to_string(resp->statusCode()));
  }
}

} // namespace

TEST_CASE("S3ImageStore round-trips bytes against real MinIO", "[image_store]") {
  auto config = testConfig();
  S3ImageStore store(config, &testLoop());

  drogon::sync_wait([&]() -> drogon::Task<> {
    co_await ensureBucketExists(config);

    std::string key = "sigv4-test/roundtrip.bin";
    std::string content = "hello from the receipt scanner test suite";

    co_await store.put(key, "application/octet-stream", content);

    auto fetched = co_await store.get(key);
    REQUIRE(fetched == content);

    co_await store.remove(key);

    bool threwAfterDelete = false;
    try {
      co_await store.get(key);
    } catch (const std::exception &) {
      threwAfterDelete = true;
    }
    REQUIRE(threwAfterDelete);
  }());
}

TEST_CASE("S3ImageStore: different keys and content types round-trip independently", "[image_store]") {
  auto config = testConfig();
  S3ImageStore store(config, &testLoop());

  drogon::sync_wait([&]() -> drogon::Task<> {
    co_await ensureBucketExists(config);

    co_await store.put("sigv4-test/a.webp", "image/webp", "aaa-bytes");
    co_await store.put("sigv4-test/b.json", "application/json", "{\"b\":true}");

    REQUIRE((co_await store.get("sigv4-test/a.webp")) == "aaa-bytes");
    REQUIRE((co_await store.get("sigv4-test/b.json")) == "{\"b\":true}");

    co_await store.remove("sigv4-test/a.webp");
    co_await store.remove("sigv4-test/b.json");
  }());
}
