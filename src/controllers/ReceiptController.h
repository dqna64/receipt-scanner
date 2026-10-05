#pragma once

#include <drogon/HttpController.h>
#include <drogon/utils/coroutine.h>

namespace receipt_scanner {

class ReceiptController : public drogon::HttpController<ReceiptController> {
public:
  METHOD_LIST_BEGIN
  ADD_METHOD_TO(ReceiptController::upload, "/api/v1/receipts", drogon::Post,
                "receipt_scanner::filters::SessionAuthFilter");
  ADD_METHOD_TO(ReceiptController::addImages, "/api/v1/receipts/{1}/images", drogon::Post,
                "receipt_scanner::filters::SessionAuthFilter");
  ADD_METHOD_TO(ReceiptController::listImages, "/api/v1/receipts/{1}/images", drogon::Get,
                "receipt_scanner::filters::SessionAuthFilter");
  ADD_METHOD_TO(ReceiptController::list, "/api/v1/receipts", drogon::Get,
                "receipt_scanner::filters::SessionAuthFilter");
  ADD_METHOD_TO(ReceiptController::get, "/api/v1/receipts/{1}", drogon::Get,
                "receipt_scanner::filters::SessionAuthFilter");
  ADD_METHOD_TO(ReceiptController::remove, "/api/v1/receipts/{1}", drogon::Delete,
                "receipt_scanner::filters::SessionAuthFilter");
  ADD_METHOD_TO(ReceiptController::image, "/api/v1/images/{1}", drogon::Get,
                "receipt_scanner::filters::SessionAuthFilter");
  METHOD_LIST_END

  drogon::Task<> upload(drogon::HttpRequestPtr req, std::function<void(const drogon::HttpResponsePtr &)> callback);
  drogon::Task<> addImages(drogon::HttpRequestPtr req, std::function<void(const drogon::HttpResponsePtr &)> callback,
                            int64_t id);
  drogon::Task<> listImages(drogon::HttpRequestPtr req,
                             std::function<void(const drogon::HttpResponsePtr &)> callback, int64_t id);
  drogon::Task<> list(drogon::HttpRequestPtr req, std::function<void(const drogon::HttpResponsePtr &)> callback);
  drogon::Task<> get(drogon::HttpRequestPtr req, std::function<void(const drogon::HttpResponsePtr &)> callback,
                      int64_t id);
  drogon::Task<> remove(drogon::HttpRequestPtr req, std::function<void(const drogon::HttpResponsePtr &)> callback,
                         int64_t id);
  drogon::Task<> image(drogon::HttpRequestPtr req, std::function<void(const drogon::HttpResponsePtr &)> callback,
                        int64_t id);
};

} // namespace receipt_scanner
