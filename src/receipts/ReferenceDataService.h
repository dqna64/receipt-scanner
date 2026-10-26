#pragma once

#include "receipts/Errors.h"

#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>
#include <vector>

// The three user-editable lists, one service each. All per-user: a missing or not-owned id ->
// NotFoundError; a name the user already has (case-insensitive) -> ConflictError. Renames keep
// the id, so history follows live. Deletes rely on the schema's ON DELETE rules.
namespace receipt_scanner::receipts {

struct Category {
  int64_t id;
  std::string name;
  bool isIncome;
};

struct Tag {
  int64_t id;
  std::string name;
};

struct PaymentMethod {
  int64_t id;
  std::string name;
  bool storedValue;
};

// Expense and income categories, one taxonomy split by is_income. Names are unique per side,
// since the scanner only ever embeds one side's list. Deleting one leaves its items
// uncategorized (items.category_id SET NULL).
class CategoryService {
public:
  explicit CategoryService(drogon::orm::DbClientPtr db) : db_(std::move(db)) {}

  // All of userId's categories, by name.
  drogon::Task<std::vector<Category>> list(int64_t userId);
  // Body: {"name": ..., "is_income": bool (default false)}.
  drogon::Task<Category> create(int64_t userId, const nlohmann::json &body);
  // Partial. Flipping is_income is rejected while an item on a receipt of the other side uses
  // the category. The check and the write share a transaction holding the category row FOR
  // UPDATE, and item writes take FOR SHARE on it, so a concurrent item edit can't slip between.
  drogon::Task<Category> update(int64_t userId, int64_t id, const nlohmann::json &body);
  drogon::Task<> remove(int64_t userId, int64_t id);

private:
  drogon::orm::DbClientPtr db_;
};

// Item-level labels, many per item. Deleting one removes its item associations (item_tags CASCADE).
class TagService {
public:
  explicit TagService(drogon::orm::DbClientPtr db) : db_(std::move(db)) {}

  // All of userId's tags, by name.
  drogon::Task<std::vector<Tag>> list(int64_t userId);
  // Body: {"name": ...}.
  drogon::Task<Tag> create(int64_t userId, const nlohmann::json &body);
  // Body: {"name": ...} -- a rename is the only edit.
  drogon::Task<Tag> update(int64_t userId, int64_t id, const nlohmann::json &body);
  drogon::Task<> remove(int64_t userId, int64_t id);

private:
  drogon::orm::DbClientPtr db_;
};

// How a transaction was paid. stored_value marks a gift-card/Opal-style method whose purchases
// are excluded from spend (the top-up was the spend); the stats views read it live, so toggling
// it re-classifies past purchases. Deleting one sets receipts.payment_method_id NULL.
class PaymentMethodService {
public:
  explicit PaymentMethodService(drogon::orm::DbClientPtr db) : db_(std::move(db)) {}

  // All of userId's payment methods, by name.
  drogon::Task<std::vector<PaymentMethod>> list(int64_t userId);
  // Body: {"name": ..., "stored_value": bool (default false)}.
  drogon::Task<PaymentMethod> create(int64_t userId, const nlohmann::json &body);
  // Partial: {"name"?, "stored_value"?}, applied in a single UPDATE.
  drogon::Task<PaymentMethod> update(int64_t userId, int64_t id, const nlohmann::json &body);
  drogon::Task<> remove(int64_t userId, int64_t id);

private:
  drogon::orm::DbClientPtr db_;
};

} // namespace receipt_scanner::receipts
