#include "AppServices.h"

#include "config/Env.h"

#include <drogon/drogon.h>

namespace receipt_scanner::receipts {

namespace {

using receipt_scanner::config::envOr;

storage::S3ImageStore::Config s3ConfigFromEnv() {
  return storage::S3ImageStore::Config{
      .endpoint = envOr("S3_ENDPOINT", "http://localhost:9000"),
      .region = envOr("S3_REGION", "us-east-1"),
      .bucket = envOr("S3_BUCKET", "receipts"),
      .accessKey = envOr("S3_ACCESS_KEY", "receipt_scanner"),
      .secretKey = envOr("S3_SECRET_KEY", "dev_only_change_me"),
      .pathStyle = envOr("S3_PATH_STYLE", "true") == "true",
  };
}

} // namespace

storage::ImageNormalizer &imageNormalizer() {
  static storage::ImageNormalizer instance;
  return instance;
}

storage::S3ImageStore &imageStore() {
  static storage::S3ImageStore instance(s3ConfigFromEnv(), drogon::app().getLoop());
  return instance;
}

} // namespace receipt_scanner::receipts
