#pragma once

#include <string>

namespace receipt_scanner::auth {

// Argon2id via libsodium's crypto_pwhash_str (spec Auth: "session cookie ... Password
// hashing via libsodium argon2"). The returned string is self-describing (algorithm,
// params, salt all embedded) so verify() needs nothing but the original hash string.
class PasswordHasher {
public:
  // Throws std::runtime_error on the (extremely rare) allocation/hashing failure.
  static std::string hash(const std::string &password);

  // false on ANY mismatch or malformed hash -- never throws, so a corrupt stored hash
  // fails closed (rejects login) rather than crashing the request.
  static bool verify(const std::string &password, const std::string &hash);
};

} // namespace receipt_scanner::auth
