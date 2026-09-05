#include "TokenUtils.h"

#include "SodiumInit.h"

#include <sodium.h>

#include <sstream>
#include <vector>

namespace receipt_scanner::auth {

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

} // namespace

std::string TokenUtils::generateToken(size_t byteLength) {
  ensureSodiumInitialized();
  std::vector<unsigned char> buf(byteLength);
  randombytes_buf(buf.data(), buf.size());
  return toHex(buf.data(), buf.size());
}

std::string TokenUtils::hashToken(const std::string &token) {
  ensureSodiumInitialized();
  unsigned char digest[crypto_hash_sha256_BYTES];
  crypto_hash_sha256(digest, reinterpret_cast<const unsigned char *>(token.data()), token.size());
  return toHex(digest, crypto_hash_sha256_BYTES);
}

} // namespace receipt_scanner::auth
