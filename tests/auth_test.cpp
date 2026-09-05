#include "auth/AuthService.h"
#include "config/Env.h"

#include <catch2/catch_test_macros.hpp>
#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>

using receipt_scanner::config::envOr;
using receipt_scanner::auth::AuthService;
using receipt_scanner::auth::DuplicateEmailError;

namespace {

// Same process-lifetime-client rationale as tests/db_test.cpp: a fresh DbClient per
// TEST_CASE deadlocks on teardown (its own I/O thread's callback appears to trigger the
// destructor, which then tries to join that same thread).
const drogon::orm::DbClientPtr &testClient() {
  static drogon::orm::DbClientPtr client = drogon::orm::DbClient::newPgClient(
      "host=" + envOr("DB_HOST", "localhost") + " port=" + envOr("DB_PORT", "5432") +
          " dbname=" + envOr("DB_NAME", "receipt_scanner") + " user=" + envOr("DB_USER", "receipt_scanner") +
          " password=" + envOr("DB_PASSWORD", ""),
      /*connSize=*/1);
  return client;
}

drogon::Task<> wipeAuthTables(const drogon::orm::DbClientPtr &db) {
  co_await db->execSqlCoro("TRUNCATE users, categories, payment_methods, sessions, auth_tokens RESTART IDENTITY CASCADE");
}

} // namespace

TEST_CASE("registration seeds categories + cash payment method, in one transaction", "[auth]") {
  const auto &db = testClient();
  AuthService authService(db);

  drogon::sync_wait([&]() -> drogon::Task<> {
    co_await wipeAuthTables(db);

    auto user = co_await authService.registerUser("newuser@example.com", "a-fine-password");

    auto categoryCount =
        co_await db->execSqlCoro("SELECT count(*) AS n, count(*) FILTER (WHERE is_income) AS income_n "
                                  "FROM categories WHERE user_id = $1",
                                  user.userId);
    REQUIRE(categoryCount[0]["n"].as<int64_t>() == 22); // 18 expense + 4 income, spec seed lists
    REQUIRE(categoryCount[0]["income_n"].as<int64_t>() == 4);

    auto paymentMethods =
        co_await db->execSqlCoro("SELECT name, stored_value FROM payment_methods WHERE user_id = $1", user.userId);
    REQUIRE(paymentMethods.size() == 1);
    REQUIRE(paymentMethods[0]["name"].as<std::string>() == "cash");
    REQUIRE(paymentMethods[0]["stored_value"].as<bool>() == false);

    co_await wipeAuthTables(db);
  }());
}

TEST_CASE("duplicate email is rejected, not a 500-worthy crash", "[auth]") {
  const auto &db = testClient();
  AuthService authService(db);

  drogon::sync_wait([&]() -> drogon::Task<> {
    co_await wipeAuthTables(db);

    co_await authService.registerUser("dup@example.com", "first-password");

    bool threw = false;
    try {
      co_await authService.registerUser("dup@example.com", "second-password");
    } catch (const DuplicateEmailError &) {
      threw = true;
    }
    REQUIRE(threw);

    // Exactly one user should exist -- the failed attempt's transaction must have rolled
    // back cleanly (rather than, say, half-committing seeded categories for a phantom user).
    auto count = co_await db->execSqlCoro("SELECT count(*) AS n FROM users WHERE email = 'dup@example.com'");
    REQUIRE(count[0]["n"].as<int64_t>() == 1);

    co_await wipeAuthTables(db);
  }());
}

TEST_CASE("two independent signups are isolated", "[auth]") {
  const auto &db = testClient();
  AuthService authService(db);

  drogon::sync_wait([&]() -> drogon::Task<> {
    co_await wipeAuthTables(db);

    auto userA = co_await authService.registerUser("a@example.com", "password-a");
    auto userB = co_await authService.registerUser("b@example.com", "password-b");
    REQUIRE(userA.userId != userB.userId);

    // Each has its own 22 seeded categories -- not sharing rows, not skipping the seed
    // because "a taxonomy already exists".
    auto countA = co_await db->execSqlCoro("SELECT count(*) AS n FROM categories WHERE user_id = $1", userA.userId);
    auto countB = co_await db->execSqlCoro("SELECT count(*) AS n FROM categories WHERE user_id = $1", userB.userId);
    REQUIRE(countA[0]["n"].as<int64_t>() == 22);
    REQUIRE(countB[0]["n"].as<int64_t>() == 22);

    // No category row leaks across the two tenants.
    auto crossCheck = co_await db->execSqlCoro(
        "SELECT count(*) AS n FROM categories WHERE user_id = $1 AND id IN "
        "(SELECT id FROM categories WHERE user_id = $2)",
        userA.userId, userB.userId);
    REQUIRE(crossCheck[0]["n"].as<int64_t>() == 0);

    co_await wipeAuthTables(db);
  }());
}

TEST_CASE("login: wrong password and unknown email both reject uniformly", "[auth]") {
  const auto &db = testClient();
  AuthService authService(db);

  drogon::sync_wait([&]() -> drogon::Task<> {
    co_await wipeAuthTables(db);
    co_await authService.registerUser("real@example.com", "the-real-password");

    auto wrongPassword = co_await authService.login("real@example.com", "not-the-password");
    REQUIRE_FALSE(wrongPassword.has_value());

    auto unknownEmail = co_await authService.login("nobody@example.com", "anything");
    REQUIRE_FALSE(unknownEmail.has_value());

    auto correct = co_await authService.login("real@example.com", "the-real-password");
    REQUIRE(correct.has_value());
    REQUIRE_FALSE(correct->rawToken.empty());

    co_await wipeAuthTables(db);
  }());
}

TEST_CASE("session validation: valid, expired, and logged-out sessions", "[auth]") {
  const auto &db = testClient();
  AuthService authService(db);

  drogon::sync_wait([&]() -> drogon::Task<> {
    co_await wipeAuthTables(db);
    auto user = co_await authService.registerUser("session-test@example.com", "password123");
    auto session = co_await authService.login("session-test@example.com", "password123");
    REQUIRE(session.has_value());

    auto validated = co_await authService.validateSession(session->rawToken);
    REQUIRE(validated.has_value());
    REQUIRE(*validated == user.userId);

    // Directly age the session's expiry into the past (spec Invariants territory: expiry
    // is a DB fact, not something the service can be tricked about from the outside).
    co_await db->execSqlCoro("UPDATE sessions SET expires_at = now() - interval '1 second'");
    auto expiredResult = co_await authService.validateSession(session->rawToken);
    REQUIRE_FALSE(expiredResult.has_value());

    // A fresh session, then logout, then it should no longer validate.
    auto session2 = co_await authService.login("session-test@example.com", "password123");
    REQUIRE(session2.has_value());
    co_await authService.logout(session2->rawToken);
    auto afterLogout = co_await authService.validateSession(session2->rawToken);
    REQUIRE_FALSE(afterLogout.has_value());

    // An outright unknown token never validates.
    auto bogus = co_await authService.validateSession("not-a-real-token");
    REQUIRE_FALSE(bogus.has_value());

    co_await wipeAuthTables(db);
  }());
}

TEST_CASE("password reset invalidates all existing sessions (decided 2c)", "[auth]") {
  const auto &db = testClient();
  AuthService authService(db);

  drogon::sync_wait([&]() -> drogon::Task<> {
    co_await wipeAuthTables(db);
    co_await authService.registerUser("reset-test@example.com", "old-password");

    auto session = co_await authService.login("reset-test@example.com", "old-password");
    REQUIRE(session.has_value());
    REQUIRE((co_await authService.validateSession(session->rawToken)).has_value());

    auto resetToken = co_await authService.requestPasswordReset("reset-test@example.com");
    auto confirmed = co_await authService.confirmPasswordReset(resetToken, "new-password");
    REQUIRE(confirmed);

    // The session that predates the reset must be dead now.
    REQUIRE_FALSE((co_await authService.validateSession(session->rawToken)).has_value());

    // Old password no longer works; new one does.
    REQUIRE_FALSE((co_await authService.login("reset-test@example.com", "old-password")).has_value());
    REQUIRE((co_await authService.login("reset-test@example.com", "new-password")).has_value());

    // The reset token is single-use.
    auto reuse = co_await authService.confirmPasswordReset(resetToken, "another-password");
    REQUIRE_FALSE(reuse);

    co_await wipeAuthTables(db);
  }());
}

TEST_CASE("email verification token confirms and is single-use", "[auth]") {
  const auto &db = testClient();
  AuthService authService(db);

  drogon::sync_wait([&]() -> drogon::Task<> {
    co_await wipeAuthTables(db);
    auto user = co_await authService.registerUser("verify-test@example.com", "password123");

    auto verifyToken = co_await authService.requestEmailVerification(user.userId);
    auto confirmed = co_await authService.confirmEmailVerification(verifyToken);
    REQUIRE(confirmed);

    auto row = co_await db->execSqlCoro("SELECT email_verified_at IS NOT NULL AS verified FROM users WHERE id = $1",
                                         user.userId);
    REQUIRE(row[0]["verified"].as<bool>() == true);

    auto reuse = co_await authService.confirmEmailVerification(verifyToken);
    REQUIRE_FALSE(reuse);

    co_await wipeAuthTables(db);
  }());
}
