#include "ReferenceDataService.h"

#include "receipts/JsonInput.h"
#include "receipts/Kinds.h"

namespace receipt_scanner::receipts {

namespace {

namespace in = json_input;

// Call from a catch block: rethrows the in-flight exception, as ConflictError if it was a
// unique-name violation. Matches libpq's message text because this Drogon version's async
// Postgres path throws a plain exception, not a typed UniqueViolation (as in AuthService).
[[noreturn]] void rethrowAsConflictIfDuplicate(const std::string &noun) {
  try {
    throw;
  } catch (const std::exception &e) {
    if (std::string(e.what()).find("duplicate key value violates unique constraint") != std::string::npos) {
      throw ConflictError("a " + noun + " with that name already exists");
    }
    throw;
  }
}

// A create body's "name", which is required.
std::string requiredName(const nlohmann::json &body) {
  std::optional<std::string> name;
  in::setIfPresent(body, "name", name, in::requiredString);
  if (!name) {
    throw ValidationError("name is required");
  }
  return *name;
}

Category rowToCategory(const drogon::orm::Row &row) {
  return Category{row["id"].as<int64_t>(), row["name"].as<std::string>(), row["is_income"].as<bool>()};
}

Tag rowToTag(const drogon::orm::Row &row) {
  return Tag{row["id"].as<int64_t>(), row["name"].as<std::string>()};
}

PaymentMethod rowToPaymentMethod(const drogon::orm::Row &row) {
  return PaymentMethod{row["id"].as<int64_t>(), row["name"].as<std::string>(), row["stored_value"].as<bool>()};
}

// Deletes `table` row `id` if it's userId's; NotFoundError (with `noun`) otherwise.
drogon::Task<> removeOwned(drogon::orm::DbClientPtr db, const std::string &table, const std::string &noun,
                           int64_t userId, int64_t id) {
  auto result =
      co_await db->execSqlCoro("DELETE FROM " + table + " WHERE id = $1 AND user_id = $2 RETURNING id", id, userId);
  if (result.empty()) {
    throw NotFoundError(noun + " not found");
  }
}

} // namespace

// ---- categories ----

drogon::Task<std::vector<Category>> CategoryService::list(int64_t userId) {
  auto rows = co_await db_->execSqlCoro(
      "SELECT id, name, is_income FROM categories WHERE user_id = $1 ORDER BY lower(name), id", userId);
  std::vector<Category> categories;
  for (const auto &row : rows) {
    categories.push_back(rowToCategory(row));
  }
  co_return categories;
}

drogon::Task<Category> CategoryService::create(int64_t userId, const nlohmann::json &body) {
  in::requireObject(body);
  in::rejectUnknownKeys(body, {"name", "is_income"});
  const auto name = requiredName(body);
  bool isIncome = false;
  in::setIfPresent(body, "is_income", isIncome, in::boolean);

  std::optional<drogon::orm::Result> result;
  try {
    result = co_await db_->execSqlCoro(
        "INSERT INTO categories (user_id, name, is_income) VALUES ($1, $2, $3) RETURNING id, name, is_income", userId,
        name, isIncome);
  } catch (...) {
    rethrowAsConflictIfDuplicate("category");
  }
  co_return rowToCategory((*result)[0]);
}

drogon::Task<Category> CategoryService::update(int64_t userId, int64_t id, const nlohmann::json &body) {
  in::requireObject(body);
  in::rejectUnknownKeys(body, {"name", "is_income"});
  std::optional<std::string> name;
  std::optional<bool> isIncome;
  in::setIfPresent(body, "name", name, in::requiredString);
  in::setIfPresent(body, "is_income", isIncome, in::boolean);

  std::optional<Category> updated;
  auto transaction = co_await db_->newTransactionCoro();
  try { // rollback on every non-success exit -- Drogon's Transaction commits in its destructor
    auto current = co_await transaction->execSqlCoro(
        "SELECT id, name, is_income FROM categories WHERE id = $1 AND user_id = $2 FOR UPDATE", id, userId);
    if (current.empty()) {
      throw NotFoundError("category not found");
    }
    auto category = rowToCategory(current[0]);

    if (isIncome && *isIncome != category.isIncome) {
      auto clash = co_await transaction->execSqlCoro(
          "SELECT 1 FROM items it JOIN receipts r ON r.id = it.receipt_id "
          "WHERE it.category_id = $1 AND (r.kind IN " + incomeKindsSqlList() + ") <> $2 LIMIT 1",
          id, *isIncome);
      if (!clash.empty()) {
        throw ValidationError(*isIncome ? "category is used by items on non-income receipts"
                                        : "category is used by items on income receipts");
      }
    }

    if (name) category.name = *name;
    if (isIncome) category.isIncome = *isIncome;
    co_await transaction->execSqlCoro("UPDATE categories SET name = $1, is_income = $2 WHERE id = $3", category.name,
                                      category.isIncome, id);
    updated = category;
  } catch (...) {
    transaction->rollback();
    rethrowAsConflictIfDuplicate("category");
  }
  co_return *updated;
}

drogon::Task<> CategoryService::remove(int64_t userId, int64_t id) {
  co_await removeOwned(db_, "categories", "category", userId, id);
}

// ---- tags ----

drogon::Task<std::vector<Tag>> TagService::list(int64_t userId) {
  auto rows =
      co_await db_->execSqlCoro("SELECT id, name FROM tags WHERE user_id = $1 ORDER BY lower(name), id", userId);
  std::vector<Tag> tags;
  for (const auto &row : rows) {
    tags.push_back(rowToTag(row));
  }
  co_return tags;
}

drogon::Task<Tag> TagService::create(int64_t userId, const nlohmann::json &body) {
  in::requireObject(body);
  in::rejectUnknownKeys(body, {"name"});
  const auto name = requiredName(body);

  std::optional<drogon::orm::Result> result;
  try {
    result = co_await db_->execSqlCoro("INSERT INTO tags (user_id, name) VALUES ($1, $2) RETURNING id, name", userId,
                                       name);
  } catch (...) {
    rethrowAsConflictIfDuplicate("tag");
  }
  co_return rowToTag((*result)[0]);
}

drogon::Task<Tag> TagService::update(int64_t userId, int64_t id, const nlohmann::json &body) {
  in::requireObject(body);
  in::rejectUnknownKeys(body, {"name"});
  const auto name = requiredName(body);

  std::optional<drogon::orm::Result> result;
  try {
    result = co_await db_->execSqlCoro(
        "UPDATE tags SET name = $1 WHERE id = $2 AND user_id = $3 RETURNING id, name", name, id, userId);
  } catch (...) {
    rethrowAsConflictIfDuplicate("tag");
  }
  if (result->empty()) {
    throw NotFoundError("tag not found");
  }
  co_return rowToTag((*result)[0]);
}

drogon::Task<> TagService::remove(int64_t userId, int64_t id) {
  co_await removeOwned(db_, "tags", "tag", userId, id);
}

// ---- payment methods ----

drogon::Task<std::vector<PaymentMethod>> PaymentMethodService::list(int64_t userId) {
  auto rows = co_await db_->execSqlCoro(
      "SELECT id, name, stored_value FROM payment_methods WHERE user_id = $1 ORDER BY lower(name), id", userId);
  std::vector<PaymentMethod> methods;
  for (const auto &row : rows) {
    methods.push_back(rowToPaymentMethod(row));
  }
  co_return methods;
}

drogon::Task<PaymentMethod> PaymentMethodService::create(int64_t userId, const nlohmann::json &body) {
  in::requireObject(body);
  in::rejectUnknownKeys(body, {"name", "stored_value"});
  const auto name = requiredName(body);
  bool storedValue = false;
  in::setIfPresent(body, "stored_value", storedValue, in::boolean);

  std::optional<drogon::orm::Result> result;
  try {
    result = co_await db_->execSqlCoro("INSERT INTO payment_methods (user_id, name, stored_value) VALUES ($1, $2, $3) "
                                       "RETURNING id, name, stored_value",
                                       userId, name, storedValue);
  } catch (...) {
    rethrowAsConflictIfDuplicate("payment method");
  }
  co_return rowToPaymentMethod((*result)[0]);
}

drogon::Task<PaymentMethod> PaymentMethodService::update(int64_t userId, int64_t id, const nlohmann::json &body) {
  in::requireObject(body);
  in::rejectUnknownKeys(body, {"name", "stored_value"});
  std::optional<std::string> name;
  std::optional<bool> storedValue;
  in::setIfPresent(body, "name", name, in::requiredString);
  in::setIfPresent(body, "stored_value", storedValue, in::boolean);

  // One statement, so absent fields keep their current value atomically -- no read-then-write.
  std::optional<drogon::orm::Result> result;
  try {
    result = co_await db_->execSqlCoro(
        "UPDATE payment_methods SET name = COALESCE($1, name), stored_value = COALESCE($2, stored_value) "
        "WHERE id = $3 AND user_id = $4 RETURNING id, name, stored_value",
        name, storedValue, id, userId);
  } catch (...) {
    rethrowAsConflictIfDuplicate("payment method");
  }
  if (result->empty()) {
    throw NotFoundError("payment method not found");
  }
  co_return rowToPaymentMethod((*result)[0]);
}

drogon::Task<> PaymentMethodService::remove(int64_t userId, int64_t id) {
  co_await removeOwned(db_, "payment_methods", "payment method", userId, id);
}

} // namespace receipt_scanner::receipts
