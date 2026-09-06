#pragma once

#include <chrono>
#include <string>

namespace receipt_scanner::storage {

// AWS Signature Version 4 (header-based auth), implemented from the algorithm spec directly
// -- canonical request -> string-to-sign -> HMAC chain -- rather than pulling in the AWS SDK
// (spec Architecture: the SDK was rejected as a huge build tax for ~150 lines of OpenSSL/
// libsodium HMAC). Deliberately separated from S3ImageStore/HTTP so the algorithm itself is
// directly unit-testable (see tests/sigv4_test.cpp) without a network round-trip.
class Sigv4Signer {
public:
  Sigv4Signer(std::string accessKey, std::string secretKey, std::string region, std::string service = "s3");

  struct SignedHeaders {
    std::string authorization;   // full "Authorization" header value
    std::string amzDate;         // "x-amz-date" header value (also embedded in the signature)
    std::string contentSha256;   // "x-amz-content-sha256" header value (also embedded)
  };

  // canonicalUri: URI-encoded absolute path, e.g. "/my-bucket/receipts/abc123.webp" (path-style)
  // or "/receipts/abc123.webp" (virtual-hosted, bucket already in `host`).
  // host: the Host header value, e.g. "minio:9000" or "my-bucket.s3.amazonaws.com".
  SignedHeaders sign(const std::string &method, const std::string &canonicalUri, const std::string &host,
                      const std::string &payload,
                      std::chrono::system_clock::time_point now = std::chrono::system_clock::now()) const;

private:
  std::string accessKey_;
  std::string secretKey_;
  std::string region_;
  std::string service_;
};

} // namespace receipt_scanner::storage
