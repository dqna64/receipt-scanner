#pragma once

#include <drogon/HttpController.h>
#include <drogon/utils/coroutine.h>

namespace receipt_scanner {

class ItemController : public drogon::HttpController<ItemController> {
public:
  METHOD_LIST_BEGIN
  ADD_METHOD_TO(ItemController::list, "/api/v1/receipts/{1}/items", drogon::Get,
                "receipt_scanner::filters::SessionAuthFilter");
  ADD_METHOD_TO(ItemController::create, "/api/v1/receipts/{1}/items", drogon::Post,
                "receipt_scanner::filters::SessionAuthFilter");
  ADD_METHOD_TO(ItemController::update, "/api/v1/receipts/{1}/items/{2}", drogon::Patch,
                "receipt_scanner::filters::SessionAuthFilter");
  ADD_METHOD_TO(ItemController::remove, "/api/v1/receipts/{1}/items/{2}", drogon::Delete,
                "receipt_scanner::filters::SessionAuthFilter");
  METHOD_LIST_END

  drogon::Task<> list(drogon::HttpRequestPtr req, std::function<void(const drogon::HttpResponsePtr &)> callback,
                      int64_t receiptId);
  drogon::Task<> create(drogon::HttpRequestPtr req, std::function<void(const drogon::HttpResponsePtr &)> callback,
                        int64_t receiptId);
  drogon::Task<> update(drogon::HttpRequestPtr req, std::function<void(const drogon::HttpResponsePtr &)> callback,
                        int64_t receiptId, int64_t itemId);
  drogon::Task<> remove(drogon::HttpRequestPtr req, std::function<void(const drogon::HttpResponsePtr &)> callback,
                        int64_t receiptId, int64_t itemId);
};

} // namespace receipt_scanner
