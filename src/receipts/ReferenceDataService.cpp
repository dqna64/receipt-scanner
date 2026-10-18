#include "ReferenceDataService.h"

#include "receipts/JsonInput.h"
#include "receipts/Kinds.h"

namespace receipt_scanner::receipts {

namespace {

namespace in = json_input;

struct KindInfo {
  const char *table;
  const char *flagColumn; // nullptr for tags
  const char *noun;
};

// Table, boolean column and display noun for each list.
KindInfo info(ReferenceKind kind) {
  switch (kind) {
  case ReferenceKind::Category:
    return {"categories", "is_income", "category"};
  case ReferenceKind::Tag:
    return {"tags", nullptr, "tag"};
  case ReferenceKind::PaymentMethod:
    return {"payment_methods", "stored_value", "payment method"};
  }
  throw std::logic_error("unknown ReferenceKind");
}

struct ReferenceInput {
  std::optional<std::string> name;
  std::optional<bool> flag;
};

// A create (requireName) or patch body: "name", plus the kind's boolean if it has one.
ReferenceInput parseInput(ReferenceKind kind, const nlohmann::json &body, bool requireName) {
  in::requireObject(body);
  const auto i = info(kind);
  if (i.flagColumn) {
    in::rejectUnknownKeys(body, {"name", i.flagColumn});
  } else {
    in::rejectUnknownKeys(body, {"name"});
  }
  if (requireName && !body.contains("name")) {
    throw ValidationError("name is required");
  }

  ReferenceInput input;
  in::setIfPresent(body, "name", input.name, in::requiredString);
  if (i.flagColumn) {
    in::setIfPresent(body, i.flagColumn, input.flag, in::boolean);
  }
  return input;
}

// "id, name[, <flag> AS flag]" -- the boolean is aliased so rowToEntry reads one name for all three.
std::string selectColumns(const KindInfo &i) {
  return std::string("id, name") + (i.flagColumn ? std::string(", ") + i.flagColumn + " AS flag" : "");
}

// A row selected with selectColumns -> ReferenceEntry.
ReferenceEntry rowToEntry(const drogon::orm::Row &row, const KindInfo &i) {
  return ReferenceEntry{row["id"].as<int64_t>(), row["name"].as<std::string>(),
                        i.flagColumn ? std::optional<bool>(row["flag"].as<bool>()) : std::nullopt};
}

// Call from a catch block: rethrows the in-flight exception, as a ConflictError if it was a
// unique-name violation. Same detection as AuthService::registerUser: this Drogon version's async Postgres path throws a
// plain exception carrying libpq's text, not a typed UniqueViolation.
[[noreturn]] void rethrowAsConflictIfDuplicate(const KindInfo &i) {
  try {
    throw;
  } catch (const std::exception &e) {
    if (std::string(e.what()).find("duplicate key value violates unique constraint") != std::string::npos) {
      throw ConflictError(std::string("a ") + i.noun + " with that name already exists");
    }
    throw;
  }
}

} // namespace

std::optional<std::string> ReferenceDataService::flagField(ReferenceKind kind) {
  const auto i = info(kind);
  return i.flagColumn ? std::optional<std::string>(i.flagColumn) : std::nullopt;
}

drogon::Task<std::vector<ReferenceEntry>> ReferenceDataService::list(ReferenceKind kind, int64_t userId) {
  const auto i = info(kind);
  auto result = co_await db_->execSqlCoro(
      "SELECT " + selectColumns(i) + " FROM " + i.table + " WHERE user_id = $1 ORDER BY lower(name), id", userId);
  std::vector<ReferenceEntry> entries;
  entries.reserve(result.size());
  for (const auto &row : result) {
    entries.push_back(rowToEntry(row, i));
  }
  co_return entries;
}

drogon::Task<ReferenceEntry> ReferenceDataService::create(ReferenceKind kind, int64_t userId, const nlohmann::json &body) {
  const auto i = info(kind);
  auto input = parseInput(kind, body, /*requireName=*/true);

  std::optional<drogon::orm::Result> result;
  try {
    if (i.flagColumn) {
      result = co_await db_->execSqlCoro(std::string("INSERT INTO ") + i.table + " (user_id, name, " + i.flagColumn +
                                             ") VALUES ($1, $2, $3) RETURNING " + selectColumns(i),
                                         userId, *input.name, input.flag.value_or(false));
    } else {
      result = co_await db_->execSqlCoro(std::string("INSERT INTO ") + i.table +
                                             " (user_id, name) VALUES ($1, $2) RETURNING " + selectColumns(i),
                                         userId, *input.name);
    }
  } catch (...) {
    rethrowAsConflictIfDuplicate(i);
  }
  co_return rowToEntry((*result)[0], i);
}

drogon::Task<ReferenceEntry> ReferenceDataService::update(ReferenceKind kind, int64_t userId, int64_t id,
                                                          const nlohmann::json &body) {
  const auto i = info(kind);
  auto input = parseInput(kind, body, /*requireName=*/false);

  auto current = co_await db_->execSqlCoro(
      "SELECT " + selectColumns(i) + " FROM " + i.table + " WHERE id = $1 AND user_id = $2", id, userId);
  if (current.empty()) {
    throw NotFoundError(std::string(i.noun) + " not found");
  }
  auto entry = rowToEntry(current[0], i);

  if (kind == ReferenceKind::Category && input.flag && *input.flag != *entry.flag) {
    auto clash = co_await db_->execSqlCoro(
        "SELECT 1 FROM items it JOIN receipts r ON r.id = it.receipt_id "
        "WHERE it.category_id = $1 AND (r.kind IN " + incomeKindsSqlList() + ") <> $2 LIMIT 1",
        id, *input.flag);
    if (!clash.empty()) {
      throw ValidationError(*input.flag ? "category is used by items on non-income receipts"
                                        : "category is used by items on income receipts");
    }
  }

  if (input.name) entry.name = *input.name;
  if (input.flag) entry.flag = input.flag;

  try {
    if (i.flagColumn) {
      co_await db_->execSqlCoro(std::string("UPDATE ") + i.table + " SET name = $1, " + i.flagColumn +
                                    " = $2 WHERE id = $3",
                                entry.name, *entry.flag, id);
    } else {
      co_await db_->execSqlCoro(std::string("UPDATE ") + i.table + " SET name = $1 WHERE id = $2", entry.name, id);
    }
  } catch (...) {
    rethrowAsConflictIfDuplicate(i);
  }
  co_return entry;
}

drogon::Task<> ReferenceDataService::remove(ReferenceKind kind, int64_t userId, int64_t id) {
  const auto i = info(kind);
  auto result = co_await db_->execSqlCoro(
      std::string("DELETE FROM ") + i.table + " WHERE id = $1 AND user_id = $2 RETURNING id", id, userId);
  if (result.empty()) {
    throw NotFoundError(std::string(i.noun) + " not found");
  }
}

} // namespace receipt_scanner::receipts
