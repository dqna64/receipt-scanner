#pragma once

#include <drogon/HttpController.h>
#include <drogon/utils/coroutine.h>

namespace receipt_scanner {

// Categories, tags and payment methods: the same four routes each, all backed by
// ReferenceDataService. Drogon binds one member function per route, hence the thin wrappers.
class ReferenceDataController : public drogon::HttpController<ReferenceDataController> {
public:
  using Callback = std::function<void(const drogon::HttpResponsePtr &)>;

  METHOD_LIST_BEGIN
  ADD_METHOD_TO(ReferenceDataController::listCategories, "/api/v1/categories", drogon::Get,
                "receipt_scanner::filters::SessionAuthFilter");
  ADD_METHOD_TO(ReferenceDataController::createCategory, "/api/v1/categories", drogon::Post,
                "receipt_scanner::filters::SessionAuthFilter");
  ADD_METHOD_TO(ReferenceDataController::updateCategory, "/api/v1/categories/{1}", drogon::Patch,
                "receipt_scanner::filters::SessionAuthFilter");
  ADD_METHOD_TO(ReferenceDataController::removeCategory, "/api/v1/categories/{1}", drogon::Delete,
                "receipt_scanner::filters::SessionAuthFilter");
  ADD_METHOD_TO(ReferenceDataController::listTags, "/api/v1/tags", drogon::Get,
                "receipt_scanner::filters::SessionAuthFilter");
  ADD_METHOD_TO(ReferenceDataController::createTag, "/api/v1/tags", drogon::Post,
                "receipt_scanner::filters::SessionAuthFilter");
  ADD_METHOD_TO(ReferenceDataController::updateTag, "/api/v1/tags/{1}", drogon::Patch,
                "receipt_scanner::filters::SessionAuthFilter");
  ADD_METHOD_TO(ReferenceDataController::removeTag, "/api/v1/tags/{1}", drogon::Delete,
                "receipt_scanner::filters::SessionAuthFilter");
  ADD_METHOD_TO(ReferenceDataController::listPaymentMethods, "/api/v1/payment-methods", drogon::Get,
                "receipt_scanner::filters::SessionAuthFilter");
  ADD_METHOD_TO(ReferenceDataController::createPaymentMethod, "/api/v1/payment-methods", drogon::Post,
                "receipt_scanner::filters::SessionAuthFilter");
  ADD_METHOD_TO(ReferenceDataController::updatePaymentMethod, "/api/v1/payment-methods/{1}", drogon::Patch,
                "receipt_scanner::filters::SessionAuthFilter");
  ADD_METHOD_TO(ReferenceDataController::removePaymentMethod, "/api/v1/payment-methods/{1}", drogon::Delete,
                "receipt_scanner::filters::SessionAuthFilter");
  METHOD_LIST_END

  drogon::Task<> listCategories(drogon::HttpRequestPtr req, Callback callback);
  drogon::Task<> createCategory(drogon::HttpRequestPtr req, Callback callback);
  drogon::Task<> updateCategory(drogon::HttpRequestPtr req, Callback callback, int64_t id);
  drogon::Task<> removeCategory(drogon::HttpRequestPtr req, Callback callback, int64_t id);
  drogon::Task<> listTags(drogon::HttpRequestPtr req, Callback callback);
  drogon::Task<> createTag(drogon::HttpRequestPtr req, Callback callback);
  drogon::Task<> updateTag(drogon::HttpRequestPtr req, Callback callback, int64_t id);
  drogon::Task<> removeTag(drogon::HttpRequestPtr req, Callback callback, int64_t id);
  drogon::Task<> listPaymentMethods(drogon::HttpRequestPtr req, Callback callback);
  drogon::Task<> createPaymentMethod(drogon::HttpRequestPtr req, Callback callback);
  drogon::Task<> updatePaymentMethod(drogon::HttpRequestPtr req, Callback callback, int64_t id);
  drogon::Task<> removePaymentMethod(drogon::HttpRequestPtr req, Callback callback, int64_t id);
};

} // namespace receipt_scanner
