#pragma once

#include "receipts/Errors.h"
#include "receipts/LedgerInput.h"

#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace receipt_scanner::receipts {

struct Item {
  int64_t id;
  int64_t receiptId;
  std::optional<std::string> name;
  std::optional<double> quantity;
  std::optional<int64_t> unitPriceCents;
  int64_t amountCents;
  std::optional<int64_t> categoryId;
  bool informational;
  std::vector<int64_t> tagIds;
};

// Items carry no user_id: tenancy comes from the owning receipt, so every method resolves the
// receipt under userId first (missing or not-owned -> ReceiptNotFoundError), and an item id
// that isn't on that receipt -> NotFoundError. Referenced categories/tags must belong to userId
// (ValidationError otherwise -- the schema's FKs alone would happily accept another tenant's id).
class ItemService {
public:
  explicit ItemService(drogon::orm::DbClientPtr db) : db_(std::move(db)) {}

  // GET /receipts/:id/items, in creation order.
  drogon::Task<std::vector<Item>> list(int64_t userId, int64_t receiptId);
  // POST /receipts/:id/items.
  drogon::Task<Item> create(int64_t userId, int64_t receiptId, const nlohmann::json &body);
  // PATCH /receipts/:id/items/:item_id -- partial (see applyItemPatch).
  drogon::Task<Item> update(int64_t userId, int64_t receiptId, int64_t itemId, const nlohmann::json &body);
  // DELETE /receipts/:id/items/:item_id. Its item_tags rows go with it (ON DELETE CASCADE).
  drogon::Task<> remove(int64_t userId, int64_t receiptId, int64_t itemId);

  // Inserts one validated item (category fit + tag ownership checked) and returns its id. Also
  // used by ReceiptService::createManual, inside its transaction.
  static drogon::Task<int64_t> insert(const std::shared_ptr<drogon::orm::Transaction> &transaction, int64_t userId,
                                      int64_t receiptId, const std::string &receiptKind, const ItemInput &item);
  // Throws ValidationError unless categoryId is userId's and its expense/income side fits kind.
  static drogon::Task<> requireCategoryFitsKind(const std::shared_ptr<drogon::orm::Transaction> &transaction,
                                                int64_t userId, int64_t categoryId, const std::string &kind);

private:
  drogon::orm::DbClientPtr db_;
};

} // namespace receipt_scanner::receipts
