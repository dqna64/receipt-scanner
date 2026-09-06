#include "Sigv4Signer.h"

#include <sodium.h>

#include <array>
#include <ctime>
#include <cstdio>

namespace receipt_scanner::storage {

namespace {

std::string toHex(const unsigned char *data, size_t len) {
  static const char *digits = "0123456789abcdef";
  std::string out;
  out.reserve(len * 2);
  for (size_t i = 0; i < len; ++i) {
    out.push_back(digits[data[i] >> 4]);
    out.push_back(digits[data[i] & 0x0F]);
  }
  return out;
}

std::string sha256Hex(const std::string &data) {
  unsigned char digest[crypto_hash_sha256_BYTES];
  crypto_hash_sha256(digest, reinterpret_cast<const unsigned char *>(data.data()), data.size());
  return toHex(digest, crypto_hash_sha256_BYTES);
}

// HMAC-SHA256 with an ARBITRARY-length key (the streaming init/update/final API, not the
// one-shot crypto_auth_hmacsha256() convenience function, which requires an exact 32-byte
// key -- SigV4's derivation chain feeds "AWS4"+secretKey, then raw 32-byte digests, as keys).
std::array<unsigned char, crypto_auth_hmacsha256_BYTES> hmac(const std::string &key, const std::string &data) {
  crypto_auth_hmacsha256_state state;
  crypto_auth_hmacsha256_init(&state, reinterpret_cast<const unsigned char *>(key.data()), key.size());
  crypto_auth_hmacsha256_update(&state, reinterpret_cast<const unsigned char *>(data.data()), data.size());
  std::array<unsigned char, crypto_auth_hmacsha256_BYTES> out{};
  crypto_auth_hmacsha256_final(&state, out.data());
  return out;
}

std::string hmacRaw(const std::string &key, const std::string &data) {
  auto digest = hmac(key, data);
  return std::string(reinterpret_cast<const char *>(digest.data()), digest.size());
}

std::string hmacHex(const std::string &key, const std::string &data) {
  auto digest = hmac(key, data);
  return toHex(digest.data(), digest.size());
}

// "20260906T153000Z" / "20260906" -- ISO 8601 basic format, UTC, as SigV4 requires.
struct Timestamps {
  std::string amzDate;  // full timestamp, used as x-amz-date and in the string-to-sign
  std::string dateStamp; // date-only, used in the credential scope and key derivation
};

Timestamps formatTimestamps(std::chrono::system_clock::time_point now) {
  std::time_t t = std::chrono::system_clock::to_time_t(now);
  std::tm utc{};
  gmtime_r(&t, &utc);

  std::array<char, 17> amzDateBuf{};
  std::strftime(amzDateBuf.data(), amzDateBuf.size(), "%Y%m%dT%H%M%SZ", &utc);

  std::array<char, 9> dateStampBuf{};
  std::strftime(dateStampBuf.data(), dateStampBuf.size(), "%Y%m%d", &utc);

  return Timestamps{std::string(amzDateBuf.data()), std::string(dateStampBuf.data())};
}

} // namespace

Sigv4Signer::Sigv4Signer(std::string accessKey, std::string secretKey, std::string region, std::string service)
    : accessKey_(std::move(accessKey)), secretKey_(std::move(secretKey)), region_(std::move(region)),
      service_(std::move(service)) {}

Sigv4Signer::SignedHeaders Sigv4Signer::sign(const std::string &method, const std::string &canonicalUri,
                                              const std::string &host, const std::string &payload,
                                              std::chrono::system_clock::time_point now) const {
  const auto [amzDate, dateStamp] = formatTimestamps(now);
  const auto payloadHash = sha256Hex(payload);

  // Canonical request (AWS docs, "Create a canonical request"): method, URI, query string
  // (always empty here -- ImageStore never needs one), canonical headers, signed headers,
  // hashed payload. Headers must be lowercase and alphabetically sorted -- "host" sorts
  // before "x-amz-content-sha256" before "x-amz-date" already, so no explicit sort needed.
  const std::string canonicalHeaders =
      "host:" + host + "\n" + "x-amz-content-sha256:" + payloadHash + "\n" + "x-amz-date:" + amzDate + "\n";
  const std::string signedHeaders = "host;x-amz-content-sha256;x-amz-date";
  const std::string canonicalRequest = method + "\n" + canonicalUri + "\n" + "" /* query string */ + "\n" +
                                        canonicalHeaders + "\n" + signedHeaders + "\n" + payloadHash;

  const std::string credentialScope = dateStamp + "/" + region_ + "/" + service_ + "/aws4_request";
  const std::string stringToSign =
      "AWS4-HMAC-SHA256\n" + amzDate + "\n" + credentialScope + "\n" + sha256Hex(canonicalRequest);

  // Signing-key derivation chain (AWS docs, "Derive a signing key"): each step's raw digest
  // (not hex) becomes the key for the next HMAC.
  const auto dateKey = hmacRaw("AWS4" + secretKey_, dateStamp);
  const auto dateRegionKey = hmacRaw(dateKey, region_);
  const auto dateRegionServiceKey = hmacRaw(dateRegionKey, service_);
  const auto signingKey = hmacRaw(dateRegionServiceKey, "aws4_request");
  const auto signature = hmacHex(signingKey, stringToSign);

  const std::string authorization = "AWS4-HMAC-SHA256 Credential=" + accessKey_ + "/" + credentialScope +
                                     ", SignedHeaders=" + signedHeaders + ", Signature=" + signature;

  return SignedHeaders{authorization, amzDate, payloadHash};
}

} // namespace receipt_scanner::storage
