#pragma once

#include "receipts/Errors.h"

#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace receipt_scanner::receipts {

// The three user-editable lists. Same shape apart from one boolean: categories.is_income
// (expense vs income taxonomy), payment_methods.stored_value (spec stored-value convention);
// tags have none.
enum class ReferenceKind { Category, Tag, PaymentMethod };

struct ReferenceEntry {
  int64_t id;
  std::string name;
  std::optional<bool> flag; // nullopt for tags
};

// Per-user CRUD. Missing or not-owned id -> NotFoundError; a name already used by this user
// (case-insensitive; per expense/income side for categories) -> ConflictError. Deletes rely on
// the schema's ON DELETE rules: items.category_id and receipts.payment_method_id SET NULL,
// item_tags CASCADE. Renames keep the id, so history follows live (spec Review).
class ReferenceDataService {
public:
  explicit ReferenceDataService(drogon::orm::DbClientPtr db) : db_(std::move(db)) {}

  // All of userId's entries, by name (case-insensitive).
  drogon::Task<std::vector<ReferenceEntry>> list(ReferenceKind kind, int64_t userId);
  // Body: {"name": ..., <flag>: bool (default false)}.
  drogon::Task<ReferenceEntry> create(ReferenceKind kind, int64_t userId, const nlohmann::json &body);
  // Flipping a category's is_income is rejected while any item on a receipt of the other side
  // uses it (an income category on a purchase's item, or vice versa).
  drogon::Task<ReferenceEntry> update(ReferenceKind kind, int64_t userId, int64_t id, const nlohmann::json &body);
  // Referencing rows follow the schema's ON DELETE rules (see the class comment).
  drogon::Task<> remove(ReferenceKind kind, int64_t userId, int64_t id);

  // JSON name of the kind's boolean ("is_income" / "stored_value"), nullopt for tags.
  static std::optional<std::string> flagField(ReferenceKind kind);

private:
  drogon::orm::DbClientPtr db_;
};

} // namespace receipt_scanner::receipts
