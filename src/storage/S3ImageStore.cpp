#include "S3ImageStore.h"

#include <drogon/drogon.h>

#include <stdexcept>

namespace receipt_scanner::storage {

namespace {

// AWS's UriEncode (percent-encode everything except unreserved chars A-Z a-z 0-9 - . _ ~;
// '/' is preserved since we're encoding a full path, not an individual segment/key name).
// Deliberately hand-rolled rather than relying on a platform URI-encode function: AWS's own
// docs warn standard library encoders differ subtly (case of hex digits, which chars are
// "unreserved") in ways that silently break the signature.
std::string uriEncodePath(const std::string &path) {
  static const char *hex = "0123456789ABCDEF";
  std::string out;
  out.reserve(path.size());
  for (unsigned char c : path) {
    bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
                       c == '.' || c == '_' || c == '~';
    if (unreserved || c == '/') {
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back('%');
      out.push_back(hex[c >> 4]);
      out.push_back(hex[c & 0x0F]);
    }
  }
  return out;
}

void addSignedHeaders(const drogon::HttpRequestPtr &req, const std::string &host,
                       const Sigv4Signer::SignedHeaders &signed_) {
  req->addHeader("Host", host);
  req->addHeader("x-amz-date", signed_.amzDate);
  req->addHeader("x-amz-content-sha256", signed_.contentSha256);
  req->addHeader("Authorization", signed_.authorization);
  // We already URI-encoded the path ourselves to match exactly what was signed -- Drogon's
  // own default path-encoding would double-encode it (different rules, e.g. for '/'),
  // corrupting the request out from under a signature that's already been computed for it.
  req->setPathEncode(false);
}

std::runtime_error httpError(const char *op, const drogon::HttpResponsePtr &resp) {
  return std::runtime_error(std::string("S3ImageStore::") + op + " failed: HTTP " +
                             std::to_string(resp->statusCode()) + " " + std::string(resp->body()));
}

} // namespace

S3ImageStore::S3ImageStore(Config config, trantor::EventLoop *loop)
    : config_(config), signer_(config.accessKey, config.secretKey, config.region, "s3"),
      client_(drogon::HttpClient::newHttpClient(config.endpoint, loop)) {}

std::string S3ImageStore::hostHeader() const {
  // Strip the scheme -- the Host header / SigV4 signing string is host[:port] only.
  auto schemeEnd = config_.endpoint.find("://");
  auto hostAndPort = schemeEnd == std::string::npos ? config_.endpoint : config_.endpoint.substr(schemeEnd + 3);
  return config_.pathStyle ? hostAndPort : config_.bucket + "." + hostAndPort;
}

std::string S3ImageStore::canonicalUri(const std::string &key) const {
  std::string path = config_.pathStyle ? "/" + config_.bucket + "/" + key : "/" + key;
  return uriEncodePath(path);
}

drogon::Task<> S3ImageStore::put(std::string key, std::string contentType, std::string bytes) {
  auto uri = canonicalUri(key);
  auto host = hostHeader();
  auto signed_ = signer_.sign("PUT", uri, host, bytes);

  auto req = drogon::HttpRequest::newHttpRequest();
  req->setMethod(drogon::Put);
  req->setPath(uri);
  addSignedHeaders(req, host, signed_);
  req->setContentTypeString(contentType);
  req->setBody(std::move(bytes));

  auto resp = co_await client_->sendRequestCoro(req);
  if (resp->statusCode() != drogon::k200OK) {
    throw httpError("put", resp);
  }
}

drogon::Task<std::string> S3ImageStore::get(std::string key) {
  auto uri = canonicalUri(key);
  auto host = hostHeader();
  auto signed_ = signer_.sign("GET", uri, host, "");

  auto req = drogon::HttpRequest::newHttpRequest();
  req->setMethod(drogon::Get);
  req->setPath(uri);
  addSignedHeaders(req, host, signed_);

  auto resp = co_await client_->sendRequestCoro(req);
  if (resp->statusCode() != drogon::k200OK) {
    throw httpError("get", resp);
  }
  co_return std::string(resp->body());
}

drogon::Task<> S3ImageStore::remove(std::string key) {
  auto uri = canonicalUri(key);
  auto host = hostHeader();
  auto signed_ = signer_.sign("DELETE", uri, host, "");

  auto req = drogon::HttpRequest::newHttpRequest();
  req->setMethod(drogon::Delete);
  req->setPath(uri);
  addSignedHeaders(req, host, signed_);

  auto resp = co_await client_->sendRequestCoro(req);
  // S3/MinIO DELETE returns 204 No Content on success.
  if (resp->statusCode() != drogon::k204NoContent && resp->statusCode() != drogon::k200OK) {
    throw httpError("remove", resp);
  }
}

} // namespace receipt_scanner::storage
