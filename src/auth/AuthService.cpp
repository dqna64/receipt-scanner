#include "AuthService.h"

#include "PasswordHasher.h"
#include "TokenUtils.h"
#include "config/Env.h"
#include "seeds/DefaultSeeds.h"

#include <drogon/orm/Exception.h>

namespace receipt_scanner::auth {

namespace {

int64_t sessionTouchIntervalSeconds() {
  return std::stoll(receipt_scanner::config::envOr("SESSION_TOUCH_INTERVAL", "86400")); // default 1 day
}

// Binding a raw numeric C++ type where Postgres needs to CAST it to a different numeric
// type (e.g. an int64_t into a `double precision` slot for make_interval) corrupts the
// value -- observed empirically (2026-09-05): Drogon sends the parameter's native binary
// representation, but the wire-format type Postgres ends up expecting (from the cast/usage
// context) doesn't match, so the raw bytes get reinterpreted as the wrong numeric type
// (2592000 as an int64 bit pattern, read back as a double, becomes ~1.28e-317). Binding the
// interval as TEXT and letting Postgres parse it sidesteps numeric wire-format entirely.
std::string secondsToIntervalLiteral(int64_t seconds) {
  return std::to_string(seconds) + " seconds";
}

} // namespace

drogon::Task<RegisteredUser> AuthService::registerUser(std::string email, std::string password) {
  // CPU-bound, deliberately-fast argon2 (INTERACTIVE params) -- blocking the calling IO
  // thread briefly here is an accepted Step 5 tradeoff, not yet worth a worker thread pool
  // (that pattern arrives in Step 7 for genuinely heavy work like image normalization).
  auto passwordHash = PasswordHasher::hash(password);

  auto transaction = co_await db_->newTransactionCoro();

  try {
    auto userResult =
        co_await transaction->execSqlCoro("INSERT INTO users (email, password_hash) VALUES ($1, $2) RETURNING id",
                                           email, passwordHash);
    auto userId = userResult[0]["id"].as<int64_t>();

    // Per-user seeding, same transaction as the user row (spec Invariants: equal-tenant
    // registration) -- these are template constants, not global migration rows, because
    // each tenant owns and can independently edit its own copy from here on.
    for (auto name : seeds::kExpenseCategories) {
      co_await transaction->execSqlCoro("INSERT INTO categories (user_id, name, is_income) VALUES ($1, $2, false)",
                                         userId, std::string(name));
    }
    for (auto name : seeds::kIncomeCategories) {
      co_await transaction->execSqlCoro("INSERT INTO categories (user_id, name, is_income) VALUES ($1, $2, true)",
                                         userId, std::string(name));
    }
    co_await transaction->execSqlCoro("INSERT INTO payment_methods (user_id, name, stored_value) VALUES ($1, $2, false)",
                                       userId, std::string(seeds::kDefaultPaymentMethod));

    co_return RegisteredUser{userId};
  } catch (const std::exception &e) {
    // Postgres aborts the whole transaction after any error; rollback explicitly rather
    // than relying on the Transaction destructor's implicit commit racing an aborted state.
    transaction->rollback();
    // Substring match on what(), not a typed catch on drogon::orm::UniqueViolation/SqlError:
    // observed empirically (2026-09-05) that this Drogon version's async postgres path
    // (PgBatchConnection) throws a plain exception carrying the raw libpq error text, not
    // one of the typed drogon::orm::Exception.h subclasses. "duplicate key value violates
    // unique constraint" is Postgres's own fixed wording for a unique-constraint violation.
    if (std::string(e.what()).find("duplicate key value violates unique constraint") != std::string::npos) {
      throw DuplicateEmailError();
    }
    throw;
  }
}

drogon::Task<std::optional<AuthSession>> AuthService::login(std::string email, std::string password) {
  auto result = co_await db_->execSqlCoro("SELECT id, password_hash FROM users WHERE email = $1", email);
  if (result.empty()) {
    co_return std::nullopt; // unknown email: same outward result as wrong password, below
  }
  auto userId = result[0]["id"].as<int64_t>();
  auto storedHash = result[0]["password_hash"].as<std::string>();

  if (!PasswordHasher::verify(password, storedHash)) {
    co_return std::nullopt;
  }

  auto rawToken = TokenUtils::generateToken();
  auto tokenHash = TokenUtils::hashToken(rawToken);
  co_await db_->execSqlCoro(
      "INSERT INTO sessions (user_id, token_hash, expires_at) VALUES ($1, $2, now() + $3::interval)", userId,
      tokenHash, secondsToIntervalLiteral(kSessionLifetimeSeconds));

  co_return AuthSession{rawToken};
}

drogon::Task<std::optional<int64_t>> AuthService::validateSession(std::string rawToken) {
  auto tokenHash = TokenUtils::hashToken(rawToken);

  auto result = co_await db_->execSqlCoro("SELECT user_id FROM sessions WHERE token_hash = $1 AND expires_at > now()",
                                           tokenHash);
  if (result.empty()) {
    co_return std::nullopt;
  }
  auto userId = result[0]["user_id"].as<int64_t>();

  // Sliding expiry, write-throttled (spec Auth): only bump expires_at once the remaining
  // validity has decayed below (lifetime - touch_interval) -- i.e. it's more than
  // SESSION_TOUCH_INTERVAL since this session was last touched. Fire-and-forget: this
  // doesn't gate the validity result above, it just may or may not extend the session.
  const auto refreshThresholdSeconds = kSessionLifetimeSeconds - sessionTouchIntervalSeconds();
  co_await db_->execSqlCoro(
      "UPDATE sessions SET expires_at = now() + $2::interval "
      "WHERE token_hash = $1 AND expires_at < now() + $3::interval",
      tokenHash, secondsToIntervalLiteral(kSessionLifetimeSeconds), secondsToIntervalLiteral(refreshThresholdSeconds));

  co_return userId;
}

drogon::Task<> AuthService::logout(std::string rawToken) {
  auto tokenHash = TokenUtils::hashToken(rawToken);
  co_await db_->execSqlCoro("DELETE FROM sessions WHERE token_hash = $1", tokenHash);
}

drogon::Task<std::string> AuthService::requestPasswordReset(std::string email) {
  auto result = co_await db_->execSqlCoro("SELECT id FROM users WHERE email = $1", email);
  if (result.empty()) {
    // Same don't-reveal-existence principle as login: still return a token-shaped string so
    // the response shape can't distinguish this from the real path. It just won't correspond
    // to any real row, so confirmPasswordReset rejects it later.
    co_return TokenUtils::generateToken();
  }
  auto userId = result[0]["id"].as<int64_t>();

  auto rawToken = TokenUtils::generateToken();
  auto tokenHash = TokenUtils::hashToken(rawToken);
  co_await db_->execSqlCoro(
      "INSERT INTO auth_tokens (user_id, token_hash, purpose, expires_at) VALUES ($1, $2, 'reset', now() + interval '1 hour')",
      userId, tokenHash);

  co_return rawToken;
}

drogon::Task<bool> AuthService::confirmPasswordReset(std::string rawToken, std::string newPassword) {
  auto tokenHash = TokenUtils::hashToken(rawToken);

  auto result = co_await db_->execSqlCoro(
      "SELECT user_id FROM auth_tokens WHERE token_hash = $1 AND purpose = 'reset' AND expires_at > now()",
      tokenHash);
  if (result.empty()) {
    co_return false;
  }
  auto userId = result[0]["user_id"].as<int64_t>();
  auto newHash = PasswordHasher::hash(newPassword);

  auto transaction = co_await db_->newTransactionCoro();
  co_await transaction->execSqlCoro("UPDATE users SET password_hash = $1 WHERE id = $2", newHash, userId);
  // Decided 2c: a password reset invalidates ALL of the account's existing sessions -- a
  // reset implies the account may have been compromised.
  co_await transaction->execSqlCoro("DELETE FROM sessions WHERE user_id = $1", userId);
  co_await transaction->execSqlCoro("DELETE FROM auth_tokens WHERE token_hash = $1", tokenHash);

  co_return true;
}

drogon::Task<std::string> AuthService::requestEmailVerification(int64_t userId) {
  auto rawToken = TokenUtils::generateToken();
  auto tokenHash = TokenUtils::hashToken(rawToken);
  co_await db_->execSqlCoro(
      "INSERT INTO auth_tokens (user_id, token_hash, purpose, expires_at) VALUES ($1, $2, 'verify', now() + interval '24 hours')",
      userId, tokenHash);
  co_return rawToken;
}

drogon::Task<bool> AuthService::confirmEmailVerification(std::string rawToken) {
  auto tokenHash = TokenUtils::hashToken(rawToken);

  auto result = co_await db_->execSqlCoro(
      "SELECT user_id FROM auth_tokens WHERE token_hash = $1 AND purpose = 'verify' AND expires_at > now()",
      tokenHash);
  if (result.empty()) {
    co_return false;
  }
  auto userId = result[0]["user_id"].as<int64_t>();

  co_await db_->execSqlCoro("UPDATE users SET email_verified_at = now() WHERE id = $1", userId);
  co_await db_->execSqlCoro("DELETE FROM auth_tokens WHERE token_hash = $1", tokenHash);
  co_return true;
}

} // namespace receipt_scanner::auth
