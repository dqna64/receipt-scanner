#include "config/Env.h"

#include <catch2/catch_test_macros.hpp>
#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>

using receipt_scanner::config::envOr;

namespace {

// A shared, process-lifetime client rather than one per TEST_CASE: destroying a DbClientPtr
// tends to join its own I/O thread from within one of that thread's own callbacks when it's
// torn down mid-test-run, which deadlocks. One client for the whole test binary sidesteps it.
const drogon::orm::DbClientPtr &testClient() {
  static drogon::orm::DbClientPtr client = drogon::orm::DbClient::newPgClient(
      "host=" + envOr("DB_HOST", "localhost") + " port=" + envOr("DB_PORT", "5432") +
          " dbname=" + envOr("DB_NAME", "receipt_scanner") + " user=" + envOr("DB_USER", "receipt_scanner") +
          " password=" + envOr("DB_PASSWORD", ""),
      /*connSize=*/1);
  return client;
}

} // namespace

TEST_CASE("coroutine DB round-trip", "[db]") {
  const auto &db = testClient();

  drogon::sync_wait([&]() -> drogon::Task<> {
    auto result = co_await db->execSqlCoro("SELECT 1 AS one");
    REQUIRE(result.size() == 1);
    REQUIRE(result[0]["one"].as<int>() == 1);
  }());
}

// Exercises the Invariants truth table (spec) through the actual views, not just the SQL
// sanity-checked manually during Step 4: income never leaks into spend, and an
// `informational` item (payslip gross/tax/super breakdown lines, super boundary) is excluded
// from item-level sums.
TEST_CASE("Invariants: income excluded from spend, item-level informational exclusion", "[db][invariants]") {
  const auto &db = testClient();

  drogon::sync_wait([&]() -> drogon::Task<> {
    co_await db->execSqlCoro("TRUNCATE users, categories, receipts, items RESTART IDENTITY CASCADE");

    auto userResult =
        co_await db->execSqlCoro("INSERT INTO users (email, password_hash) VALUES ($1, $2) RETURNING id",
                                  "invariants-test@example.com", "hash");
    auto userId = userResult[0]["id"].as<int64_t>();

    auto catResult = co_await db->execSqlCoro(
        "INSERT INTO categories (user_id, name, is_income) VALUES ($1, $2, true) RETURNING id", userId, "salary");
    auto categoryId = catResult[0]["id"].as<int64_t>();

    auto receiptResult = co_await db->execSqlCoro(
        "INSERT INTO receipts (user_id, merchant, purchase_date, total_cents, direction, kind, source, reviewed_at) "
        "VALUES ($1, 'Employer', '2026-09-01', 300000, 'inflow', 'income', 'manual', now()) RETURNING id",
        userId);
    auto receiptId = receiptResult[0]["id"].as<int64_t>();

    co_await db->execSqlCoro(
        "INSERT INTO items (receipt_id, name, amount_cents, category_id) VALUES ($1, 'net pay', 300000, $2)",
        receiptId, categoryId);
    co_await db->execSqlCoro(
        "INSERT INTO items (receipt_id, name, amount_cents, informational) VALUES ($1, 'gross pay', 400000, true)",
        receiptId);

    auto spendCheck = co_await db->execSqlCoro("SELECT COUNT(*) AS n FROM v_countable_spend WHERE id = $1", receiptId);
    REQUIRE(spendCheck[0]["n"].as<int64_t>() == 0);

    auto incomeCheck = co_await db->execSqlCoro("SELECT COUNT(*) AS n FROM v_income WHERE id = $1", receiptId);
    REQUIRE(incomeCheck[0]["n"].as<int64_t>() == 1);

    auto itemSumCheck = co_await db->execSqlCoro(
        "SELECT COALESCE(SUM(amount_cents), 0) AS total FROM v_income_items WHERE receipt_id = $1", receiptId);
    REQUIRE(itemSumCheck[0]["total"].as<int64_t>() == 300000); // excludes the 400000 informational item

    co_await db->execSqlCoro("TRUNCATE users, categories, receipts, items RESTART IDENTITY CASCADE");
  }());
}
