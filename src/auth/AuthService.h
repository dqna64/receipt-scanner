#pragma once

#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>

#include <chrono>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>

namespace receipt_scanner::auth {

struct RegisteredUser {
  int64_t userId;
};

struct AuthSession {
  std::string rawToken; // the ONLY time the raw token exists outside the client's cookie
};

class DuplicateEmailError : public std::runtime_error {
public:
  DuplicateEmailError() : std::runtime_error("email already registered") {}
};

// The request->coroutine->DB->response business logic, kept independent of HTTP/Drogon
// controllers so it's directly unit-testable (see tests/auth_test.cpp) without a full HTTP
// harness. Controllers are thin adapters over this.
class AuthService {
public:
  // ~30-day sliding expiry (spec Auth). Exposed so the HTTP layer can set the cookie's
  // Max-Age to match, without needing to round-trip the DB's exact expires_at value.
  static constexpr int64_t kSessionLifetimeSeconds = 30LL * 24 * 60 * 60;

  explicit AuthService(drogon::orm::DbClientPtr db) : db_(std::move(db)) {}

  // Creates the user + seeds default categories/payment method in ONE transaction (spec
  // Invariants, equal-tenant registration). Throws DuplicateEmailError on a UNIQUE violation.
  drogon::Task<RegisteredUser> registerUser(std::string email, std::string password);

  // nullopt for BOTH wrong password and unknown email -- deliberately indistinguishable to
  // the caller, so this can't be used as an email-enumeration oracle.
  drogon::Task<std::optional<AuthSession>> login(std::string email, std::string password);

  // Validates a session token, applying the sliding-expiry touch rule (spec: bump expires_at
  // only when it's more than SESSION_TOUCH_INTERVAL stale, not on every request). Returns the
  // owning user_id, or nullopt if the token is missing, unknown, or expired.
  drogon::Task<std::optional<int64_t>> validateSession(std::string rawToken);

  drogon::Task<> logout(std::string rawToken);

  // Returns the raw token; the caller (HTTP layer) decides whether to email it or log it,
  // depending on the still-open email-infra decision (spec Open Eng Decisions) -- kept out
  // of this service so the service stays testable without touching env/config.
  drogon::Task<std::string> requestPasswordReset(std::string email);

  // Invalidates ALL of the user's existing sessions on success (decided 2c: a reset implies
  // the account may be compromised). Returns false if the token is invalid/expired.
  drogon::Task<bool> confirmPasswordReset(std::string rawToken, std::string newPassword);

  drogon::Task<std::string> requestEmailVerification(int64_t userId);
  drogon::Task<bool> confirmEmailVerification(std::string rawToken);

private:
  drogon::orm::DbClientPtr db_;
};

} // namespace receipt_scanner::auth
