#include "ReferenceDataController.h"

#include "controllers/Responses.h"
#include "receipts/ReferenceDataService.h"

#include <drogon/drogon.h>

namespace receipt_scanner {

namespace {

using http::currentUserId;
using http::jsonResponse;
using receipts::Category;
using receipts::CategoryService;
using receipts::PaymentMethod;
using receipts::PaymentMethodService;
using receipts::Tag;
using receipts::TagService;
using Callback = ReferenceDataController::Callback;

nlohmann::json toJson(const Category &c) {
  return nlohmann::json{{"id", c.id}, {"name", c.name}, {"is_income", c.isIncome}};
}

nlohmann::json toJson(const Tag &t) {
  return nlohmann::json{{"id", t.id}, {"name", t.name}};
}

nlohmann::json toJson(const PaymentMethod &m) {
  return nlohmann::json{{"id", m.id}, {"name", m.name}, {"stored_value", m.storedValue}};
}

template <typename T> nlohmann::json toJsonArray(const std::vector<T> &entries) {
  nlohmann::json arr = nlohmann::json::array();
  for (const auto &entry : entries) {
    arr.push_back(toJson(entry));
  }
  return arr;
}

void sendError(const Callback &callback) {
  callback(http::errorResponseFor(std::current_exception()));
}

} // namespace

// ---- categories ----

drogon::Task<> ReferenceDataController::listCategories(drogon::HttpRequestPtr req, Callback callback) {
  CategoryService service(drogon::app().getDbClient());
  try {
    auto categories = co_await service.list(currentUserId(req));
    callback(jsonResponse(drogon::k200OK, toJsonArray(categories)));
  } catch (...) {
    sendError(callback);
  }
}

drogon::Task<> ReferenceDataController::createCategory(drogon::HttpRequestPtr req, Callback callback) {
  auto body = http::parseBody(req);
  CategoryService service(drogon::app().getDbClient());
  try {
    auto category = co_await service.create(currentUserId(req), body);
    callback(jsonResponse(drogon::k201Created, toJson(category)));
  } catch (...) {
    sendError(callback);
  }
}

drogon::Task<> ReferenceDataController::updateCategory(drogon::HttpRequestPtr req, Callback callback, int64_t id) {
  auto body = http::parseBody(req);
  CategoryService service(drogon::app().getDbClient());
  try {
    auto category = co_await service.update(currentUserId(req), id, body);
    callback(jsonResponse(drogon::k200OK, toJson(category)));
  } catch (...) {
    sendError(callback);
  }
}

drogon::Task<> ReferenceDataController::removeCategory(drogon::HttpRequestPtr req, Callback callback, int64_t id) {
  CategoryService service(drogon::app().getDbClient());
  try {
    co_await service.remove(currentUserId(req), id);
    callback(jsonResponse(drogon::k200OK, {{"status", "ok"}}));
  } catch (...) {
    sendError(callback);
  }
}

// ---- tags ----

drogon::Task<> ReferenceDataController::listTags(drogon::HttpRequestPtr req, Callback callback) {
  TagService service(drogon::app().getDbClient());
  try {
    auto tags = co_await service.list(currentUserId(req));
    callback(jsonResponse(drogon::k200OK, toJsonArray(tags)));
  } catch (...) {
    sendError(callback);
  }
}

drogon::Task<> ReferenceDataController::createTag(drogon::HttpRequestPtr req, Callback callback) {
  auto body = http::parseBody(req);
  TagService service(drogon::app().getDbClient());
  try {
    auto tag = co_await service.create(currentUserId(req), body);
    callback(jsonResponse(drogon::k201Created, toJson(tag)));
  } catch (...) {
    sendError(callback);
  }
}

drogon::Task<> ReferenceDataController::updateTag(drogon::HttpRequestPtr req, Callback callback, int64_t id) {
  auto body = http::parseBody(req);
  TagService service(drogon::app().getDbClient());
  try {
    auto tag = co_await service.update(currentUserId(req), id, body);
    callback(jsonResponse(drogon::k200OK, toJson(tag)));
  } catch (...) {
    sendError(callback);
  }
}

drogon::Task<> ReferenceDataController::removeTag(drogon::HttpRequestPtr req, Callback callback, int64_t id) {
  TagService service(drogon::app().getDbClient());
  try {
    co_await service.remove(currentUserId(req), id);
    callback(jsonResponse(drogon::k200OK, {{"status", "ok"}}));
  } catch (...) {
    sendError(callback);
  }
}

// ---- payment methods ----

drogon::Task<> ReferenceDataController::listPaymentMethods(drogon::HttpRequestPtr req, Callback callback) {
  PaymentMethodService service(drogon::app().getDbClient());
  try {
    auto methods = co_await service.list(currentUserId(req));
    callback(jsonResponse(drogon::k200OK, toJsonArray(methods)));
  } catch (...) {
    sendError(callback);
  }
}

drogon::Task<> ReferenceDataController::createPaymentMethod(drogon::HttpRequestPtr req, Callback callback) {
  auto body = http::parseBody(req);
  PaymentMethodService service(drogon::app().getDbClient());
  try {
    auto method = co_await service.create(currentUserId(req), body);
    callback(jsonResponse(drogon::k201Created, toJson(method)));
  } catch (...) {
    sendError(callback);
  }
}

drogon::Task<> ReferenceDataController::updatePaymentMethod(drogon::HttpRequestPtr req, Callback callback,
                                                            int64_t id) {
  auto body = http::parseBody(req);
  PaymentMethodService service(drogon::app().getDbClient());
  try {
    auto method = co_await service.update(currentUserId(req), id, body);
    callback(jsonResponse(drogon::k200OK, toJson(method)));
  } catch (...) {
    sendError(callback);
  }
}

drogon::Task<> ReferenceDataController::removePaymentMethod(drogon::HttpRequestPtr req, Callback callback,
                                                            int64_t id) {
  PaymentMethodService service(drogon::app().getDbClient());
  try {
    co_await service.remove(currentUserId(req), id);
    callback(jsonResponse(drogon::k200OK, {{"status", "ok"}}));
  } catch (...) {
    sendError(callback);
  }
}

} // namespace receipt_scanner
