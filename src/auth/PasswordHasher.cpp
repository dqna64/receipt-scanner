#include "PasswordHasher.h"

#include "SodiumInit.h"

#include <sodium.h>

#include <array>
#include <stdexcept>

namespace receipt_scanner::auth {

std::string PasswordHasher::hash(const std::string &password) {
  ensureSodiumInitialized();
  std::array<char, crypto_pwhash_STRBYTES> out{};
  if (crypto_pwhash_str(out.data(), password.c_str(), password.size(), crypto_pwhash_OPSLIMIT_INTERACTIVE,
                         crypto_pwhash_MEMLIMIT_INTERACTIVE) != 0) {
    // Out of memory is the realistic cause -- INTERACTIVE limits are deliberately modest.
    throw std::runtime_error("PasswordHasher::hash: crypto_pwhash_str failed (out of memory?)");
  }
  return std::string(out.data());
}

bool PasswordHasher::verify(const std::string &password, const std::string &hash) {
  ensureSodiumInitialized();
  return crypto_pwhash_str_verify(hash.c_str(), password.c_str(), password.size()) == 0;
}

} // namespace receipt_scanner::auth
