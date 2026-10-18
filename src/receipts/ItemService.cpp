#include "ItemService.h"

#include "receipts/RowValues.h"

#include <map>

namespace receipt_scanner::receipts {

namespace {

using namespace row_values;

constexpr const char *kItemColumns =
    "id, receipt_id, name, quantity::float8 AS quantity, unit_price_cents, amount_cents, category_id, informational";

// A row selected with kItemColumns -> Item, tags not yet filled in.
Item rowToItem(const drogon::orm::Row &row) {
  return Item{
      row["id"].as<int64_t>(),
      row["receipt_id"].as<int64_t>(),
      optString(row, "name"),
      optDouble(row, "quantity"),
      optInt(row, "unit_price_cents"),
      row["amount_cents"].as<int64_t>(),
      optInt(row, "category_id"),
      row["informational"].as<bool>(),
      {},
  };
}

// The editable fields of a stored item, as the starting point for applyItemPatch.
ItemInput itemToInput(const Item &item) {
  return ItemInput{item.name,       item.quantity,      item.unitPriceCents, item.amountCents,
                   item.categoryId, item.informational, item.tagIds};
}

// The kind of receipt `receiptId`, which is also the ownership check for every item route:
// throws ReceiptNotFoundError unless the receipt is userId's.
//
// Transaction derives from DbClient, so these helpers run inside a transaction or on the plain
// client. The client is taken by value: passing a Transaction converts it to a temporary DbClientPtr,
// which a reference parameter would leave dangling in the coroutine frame.
drogon::Task<std::string> ownedReceiptKind(drogon::orm::DbClientPtr db, int64_t userId, int64_t receiptId) {
  auto result = co_await db->execSqlCoro("SELECT kind FROM receipts WHERE id = $1 AND user_id = $2", receiptId, userId);
  if (result.empty()) {
    throw ReceiptNotFoundError();
  }
  co_return result[0]["kind"].as<std::string>();
}

// The item's tag ids, ascending.
drogon::Task<std::vector<int64_t>> tagIdsFor(drogon::orm::DbClientPtr db, int64_t itemId) {
  auto result = co_await db->execSqlCoro("SELECT tag_id FROM item_tags WHERE item_id = $1 ORDER BY tag_id", itemId);
  std::vector<int64_t> ids;
  for (const auto &row : result) {
    ids.push_back(row["tag_id"].as<int64_t>());
  }
  co_return ids;
}

// Reads an item back with its tags -- the response body of create/update.
drogon::Task<Item> fetchItem(drogon::orm::DbClientPtr db, int64_t itemId) {
  auto result = co_await db->execSqlCoro(std::string("SELECT ") + kItemColumns + " FROM items WHERE id = $1", itemId);
  auto item = rowToItem(result[0]);
  item.tagIds = co_await tagIdsFor(db, itemId);
  co_return item;
}

// Replaces the item's whole tag set with tagIds, each of which must be userId's own tag.
drogon::Task<> replaceTags(const std::shared_ptr<drogon::orm::Transaction> &transaction, int64_t userId, int64_t itemId,
                           const std::vector<int64_t> &tagIds) {
  co_await transaction->execSqlCoro("DELETE FROM item_tags WHERE item_id = $1", itemId);
  for (auto tagId : tagIds) {
    auto owned = co_await transaction->execSqlCoro("SELECT 1 FROM tags WHERE id = $1 AND user_id = $2", tagId, userId);
    if (owned.empty()) {
      throw ValidationError("tag_ids contains a tag that does not exist");
    }
    co_await transaction->execSqlCoro("INSERT INTO item_tags (item_id, tag_id) VALUES ($1, $2)", itemId, tagId);
  }
}

} // namespace

drogon::Task<> ItemService::requireCategoryFitsKind(const std::shared_ptr<drogon::orm::Transaction> &transaction,
                                                    int64_t userId, int64_t categoryId, const std::string &kind) {
  auto result =
      co_await transaction->execSqlCoro("SELECT is_income FROM categories WHERE id = $1 AND user_id = $2", categoryId, userId);
  if (result.empty()) {
    throw ValidationError("category_id does not exist");
  }
  if (!categoryFitsKind(result[0]["is_income"].as<bool>(), kind)) {
    throw ValidationError("items on a " + kind + " need " +
                          (rulesFor(kind).usesIncomeCategories ? "an income" : "an expense") + " category");
  }
}

drogon::Task<int64_t> ItemService::insert(const std::shared_ptr<drogon::orm::Transaction> &transaction, int64_t userId,
                                          int64_t receiptId, const std::string &receiptKind, const ItemInput &item) {
  if (item.categoryId) {
    co_await requireCategoryFitsKind(transaction, userId, *item.categoryId, receiptKind);
  }
  // quantity is NUMERIC; the explicit ::float8 matches the 8-byte double Drogon binds (same
  // param-size trap as Step 8's LIMIT/OFFSET).
  auto result = co_await transaction->execSqlCoro(
      "INSERT INTO items (receipt_id, name, quantity, unit_price_cents, amount_cents, category_id, informational) "
      "VALUES ($1, $2, $3::float8, $4, $5, $6, $7) RETURNING id",
      receiptId, item.name, item.quantity, item.unitPriceCents, item.amountCents, item.categoryId, item.informational);
  auto itemId = result[0]["id"].as<int64_t>();
  co_await replaceTags(transaction, userId, itemId, item.tagIds);
  co_return itemId;
}

drogon::Task<std::vector<Item>> ItemService::list(int64_t userId, int64_t receiptId) {
  co_await ownedReceiptKind(db_, userId, receiptId);

  auto rows = co_await db_->execSqlCoro(
      std::string("SELECT ") + kItemColumns + " FROM items WHERE receipt_id = $1 ORDER BY id", receiptId);
  auto tagRows = co_await db_->execSqlCoro(
      "SELECT it.item_id, it.tag_id FROM item_tags it JOIN items i ON i.id = it.item_id "
      "WHERE i.receipt_id = $1 ORDER BY it.tag_id",
      receiptId);
  std::map<int64_t, std::vector<int64_t>> tagsByItem;
  for (const auto &row : tagRows) {
    tagsByItem[row["item_id"].as<int64_t>()].push_back(row["tag_id"].as<int64_t>());
  }

  std::vector<Item> items;
  items.reserve(rows.size());
  for (const auto &row : rows) {
    auto item = rowToItem(row);
    item.tagIds = tagsByItem[item.id];
    items.push_back(std::move(item));
  }
  co_return items;
}

drogon::Task<Item> ItemService::create(int64_t userId, int64_t receiptId, const nlohmann::json &body) {
  auto input = parseNewItem(body);

  auto transaction = co_await db_->newTransactionCoro();
  try { // rollback on every non-success exit -- Drogon's Transaction commits in its destructor
    auto kind = co_await ownedReceiptKind(transaction, userId, receiptId);
    auto itemId = co_await insert(transaction, userId, receiptId, kind, input);
    auto item = co_await fetchItem(transaction, itemId);
    co_return item;
  } catch (...) {
    transaction->rollback();
    throw;
  }
}

drogon::Task<Item> ItemService::update(int64_t userId, int64_t receiptId, int64_t itemId, const nlohmann::json &body) {
  auto transaction = co_await db_->newTransactionCoro();
  try {
    auto kind = co_await ownedReceiptKind(transaction, userId, receiptId);
    auto current = co_await transaction->execSqlCoro(
        std::string("SELECT ") + kItemColumns + " FROM items WHERE id = $1 AND receipt_id = $2 FOR UPDATE", itemId,
        receiptId);
    if (current.empty()) {
      throw NotFoundError("item not found");
    }

    auto item = itemToInput(rowToItem(current[0]));
    bool tagsGiven = false;
    applyItemPatch(item, body, tagsGiven);
    if (body.contains("category_id") && item.categoryId) {
      co_await requireCategoryFitsKind(transaction, userId, *item.categoryId, kind);
    }

    co_await transaction->execSqlCoro(
        "UPDATE items SET name = $1, quantity = $2::float8, unit_price_cents = $3, amount_cents = $4, "
        "category_id = $5, informational = $6 WHERE id = $7",
        item.name, item.quantity, item.unitPriceCents, item.amountCents, item.categoryId, item.informational, itemId);
    if (tagsGiven) {
      co_await replaceTags(transaction, userId, itemId, item.tagIds);
    }
    auto updated = co_await fetchItem(transaction, itemId);
    co_return updated;
  } catch (...) {
    transaction->rollback();
    throw;
  }
}

drogon::Task<> ItemService::remove(int64_t userId, int64_t receiptId, int64_t itemId) {
  co_await ownedReceiptKind(db_, userId, receiptId);
  auto result =
      co_await db_->execSqlCoro("DELETE FROM items WHERE id = $1 AND receipt_id = $2 RETURNING id", itemId, receiptId);
  if (result.empty()) {
    throw NotFoundError("item not found");
  }
}

} // namespace receipt_scanner::receipts
