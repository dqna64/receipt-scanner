#pragma once

#include <string>

namespace receipt_scanner::auth {

// Session/verify/reset tokens: high-entropy random values, never stored raw (spec Auth:
// "only token_hash stored, never the raw token"). Unlike passwords, these tokens are already
// unguessable, so a fast cryptographic hash (not argon2's deliberately-slow one) is the
// right tool -- it's collision/preimage resistance we need here, not brute-force resistance.
class TokenUtils {
public:
  // URL-safe-ish hex-encoded random token, `byteLength` bytes of entropy (default 32 = 256 bits).
  static std::string generateToken(size_t byteLength = 32);

  // SHA-256 hex digest of the token -- what actually gets stored/compared in the DB.
  static std::string hashToken(const std::string &token);
};

} // namespace receipt_scanner::auth
