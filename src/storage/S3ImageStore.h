#pragma once

#include "ImageStore.h"
#include "Sigv4Signer.h"

#include <drogon/HttpClient.h>

namespace receipt_scanner::storage {

// S3-compatible object storage (MinIO today, AWS S3/R2/B2 later -- spec Architecture,
// cloud-portability requirement) over Drogon's async HttpClient with hand-rolled SigV4.
class S3ImageStore : public ImageStore {
public:
  struct Config {
    std::string endpoint; // e.g. "http://minio:9000" -- no bucket, no path, scheme+host[:port] only
    std::string region;   // MinIO ignores this but SigV4 still requires a value; AWS: e.g. "ap-southeast-2"
    std::string bucket;
    std::string accessKey;
    std::string secretKey;
    // Cloud-portability requirement (spec): swapping MinIO for AWS S3/R2/B2 is a config
    // change, not a rewrite. Concretely, pointing this at real AWS S3 instead of MinIO means:
    //   endpoint   = "https://s3.<region>.amazonaws.com"   (was "http://minio:9000")
    //   region     = "<region>"                             (was whatever MinIO was given)
    //   accessKey/secretKey = the AWS IAM credentials        (was MinIO's)
    //   pathStyle  = false                                   (was true)
    // bucket stays whatever it already was. No code changes.
    bool pathStyle = true; // true: https://endpoint/bucket/key (MinIO). false: https://bucket.endpoint/key (AWS).
  };

  // `loop` drives the underlying HttpClient's I/O and MUST actually be running (nullptr
  // defaults to HttpAppFramework's loop, which only exists once drogon::app().run() has
  // started -- constructing this before then, or in a standalone test binary that never
  // calls app().run(), silently hangs every request forever with no error and nothing sent
  // on the wire, since nothing is ever polling the socket). Tests pass an explicit,
  // separately-run trantor::EventLoop (see tests/image_store_test.cpp).
  explicit S3ImageStore(Config config, trantor::EventLoop *loop = nullptr);

  drogon::Task<> put(std::string key, std::string contentType, std::string bytes) override;
  drogon::Task<std::string> get(std::string key) override;
  drogon::Task<> remove(std::string key) override;

private:
  Config config_;
  Sigv4Signer signer_;
  drogon::HttpClientPtr client_;

  std::string canonicalUri(const std::string &key) const;
  std::string hostHeader() const;
};

} // namespace receipt_scanner::storage
