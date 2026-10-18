#include "ItemController.h"

#include "controllers/Responses.h"
#include "receipts/ItemService.h"

#include <drogon/drogon.h>

namespace receipt_scanner {

namespace {

using http::currentUserId;
using http::jsonResponse;
using http::nullable;
using receipts::Item;
using receipts::ItemService;

nlohmann::json toJson(const Item &item) {
  return nlohmann::json{
      {"id", item.id},
      {"receipt_id", item.receiptId},
      {"name", nullable(item.name)},
      {"quantity", nullable(item.quantity)},
      {"unit_price_cents", nullable(item.unitPriceCents)},
      {"amount_cents", item.amountCents},
      {"category_id", nullable(item.categoryId)},
      {"informational", item.informational},
      {"tag_ids", item.tagIds},
  };
}

} // namespace

drogon::Task<> ItemController::list(drogon::HttpRequestPtr req,
                                    std::function<void(const drogon::HttpResponsePtr &)> callback, int64_t receiptId) {
  ItemService service(drogon::app().getDbClient());
  try {
    auto items = co_await service.list(currentUserId(req), receiptId);
    nlohmann::json arr = nlohmann::json::array();
    for (const auto &item : items) {
      arr.push_back(toJson(item));
    }
    callback(jsonResponse(drogon::k200OK, arr));
  } catch (...) {
    callback(http::errorResponseFor(std::current_exception()));
  }
}

drogon::Task<> ItemController::create(drogon::HttpRequestPtr req,
                                      std::function<void(const drogon::HttpResponsePtr &)> callback, int64_t receiptId) {
  auto body = http::parseBody(req);
  ItemService service(drogon::app().getDbClient());
  try {
    auto item = co_await service.create(currentUserId(req), receiptId, body);
    callback(jsonResponse(drogon::k201Created, toJson(item)));
  } catch (...) {
    callback(http::errorResponseFor(std::current_exception()));
  }
}

drogon::Task<> ItemController::update(drogon::HttpRequestPtr req,
                                      std::function<void(const drogon::HttpResponsePtr &)> callback, int64_t receiptId,
                                      int64_t itemId) {
  auto body = http::parseBody(req);
  ItemService service(drogon::app().getDbClient());
  try {
    auto item = co_await service.update(currentUserId(req), receiptId, itemId, body);
    callback(jsonResponse(drogon::k200OK, toJson(item)));
  } catch (...) {
    callback(http::errorResponseFor(std::current_exception()));
  }
}

drogon::Task<> ItemController::remove(drogon::HttpRequestPtr req,
                                      std::function<void(const drogon::HttpResponsePtr &)> callback, int64_t receiptId,
                                      int64_t itemId) {
  ItemService service(drogon::app().getDbClient());
  try {
    co_await service.remove(currentUserId(req), receiptId, itemId);
    callback(jsonResponse(drogon::k200OK, {{"status", "ok"}}));
  } catch (...) {
    callback(http::errorResponseFor(std::current_exception()));
  }
}

} // namespace receipt_scanner
