#include "storage/Sigv4Signer.h"

#include <catch2/catch_test_macros.hpp>

#include <ctime>

using receipt_scanner::storage::Sigv4Signer;

namespace {

// Fixed timestamp so the test is deterministic: 2013-05-24T00:00:00Z.
std::chrono::system_clock::time_point fixedTimestamp() {
  std::tm tm{};
  tm.tm_year = 2013 - 1900;
  tm.tm_mon = 4; // May, 0-indexed
  tm.tm_mday = 24;
  tm.tm_hour = 0;
  tm.tm_min = 0;
  tm.tm_sec = 0;
  // timegm (not mktime): interprets tm as UTC, not local time -- SigV4 timestamps are
  // always UTC.
  std::time_t t = timegm(&tm);
  return std::chrono::system_clock::from_time_t(t);
}

} // namespace

// Cross-verified independently: the exact same inputs (well-known AWS example credentials,
// deliberately NOT real ones) were run through a from-scratch reference implementation in
// Python (hashlib/hmac -- not the AWS SDK, so this isn't "does the SDK agree with itself")
// to get this expected signature, rather than trusting either implementation alone or a
// half-remembered docs example. See the algorithm itself (canonical request -> string to
// sign -> HMAC key-derivation chain) in Sigv4Signer.cpp; this test is the ground-truth check.
TEST_CASE("Sigv4Signer matches an independently-computed reference signature", "[sigv4]") {
  Sigv4Signer signer("AKIAIOSFODNN7EXAMPLE", "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY", "us-east-1", "s3");

  auto signed_ = signer.sign("GET", "/examplebucket/test.txt", "s3.amazonaws.com", "", fixedTimestamp());

  REQUIRE(signed_.amzDate == "20130524T000000Z");
  REQUIRE(signed_.contentSha256 == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  REQUIRE(signed_.authorization ==
          "AWS4-HMAC-SHA256 Credential=AKIAIOSFODNN7EXAMPLE/20130524/us-east-1/s3/aws4_request, "
          "SignedHeaders=host;x-amz-content-sha256;x-amz-date, "
          "Signature=0fcb291c4b47980ad34dd9a29532ceae67b48e45de3d6054873b430740567ec2");
}

TEST_CASE("Sigv4Signer: different payloads produce different signatures", "[sigv4]") {
  Sigv4Signer signer("AKIAIOSFODNN7EXAMPLE", "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY", "us-east-1", "s3");
  auto ts = fixedTimestamp();

  auto empty = signer.sign("PUT", "/bucket/key", "s3.amazonaws.com", "", ts);
  auto nonEmpty = signer.sign("PUT", "/bucket/key", "s3.amazonaws.com", "some bytes", ts);

  REQUIRE(empty.contentSha256 != nonEmpty.contentSha256);
  REQUIRE(empty.authorization != nonEmpty.authorization);
}
