#include "ReferenceDataController.h"

#include "controllers/Responses.h"
#include "receipts/ReferenceDataService.h"

#include <drogon/drogon.h>

namespace receipt_scanner {

namespace {

using http::currentUserId;
using http::jsonResponse;
using receipts::ReferenceDataService;
using receipts::ReferenceEntry;
using receipts::ReferenceKind;
using Callback = ReferenceDataController::Callback;

nlohmann::json toJson(ReferenceKind kind, const ReferenceEntry &entry) {
  nlohmann::json j{{"id", entry.id}, {"name", entry.name}};
  if (auto field = ReferenceDataService::flagField(kind)) {
    j[*field] = *entry.flag;
  }
  return j;
}

// The four handlers every list shares; the member functions below just pick the kind.
drogon::Task<> listEntries(ReferenceKind kind, drogon::HttpRequestPtr req, Callback callback) {
  ReferenceDataService service(drogon::app().getDbClient());
  try {
    auto entries = co_await service.list(kind, currentUserId(req));
    nlohmann::json arr = nlohmann::json::array();
    for (const auto &entry : entries) {
      arr.push_back(toJson(kind, entry));
    }
    callback(jsonResponse(drogon::k200OK, arr));
  } catch (...) {
    callback(http::errorResponseFor(std::current_exception()));
  }
}

drogon::Task<> createEntry(ReferenceKind kind, drogon::HttpRequestPtr req, Callback callback) {
  auto body = http::parseBody(req);
  ReferenceDataService service(drogon::app().getDbClient());
  try {
    auto entry = co_await service.create(kind, currentUserId(req), body);
    callback(jsonResponse(drogon::k201Created, toJson(kind, entry)));
  } catch (...) {
    callback(http::errorResponseFor(std::current_exception()));
  }
}

drogon::Task<> updateEntry(ReferenceKind kind, drogon::HttpRequestPtr req, Callback callback, int64_t id) {
  auto body = http::parseBody(req);
  ReferenceDataService service(drogon::app().getDbClient());
  try {
    auto entry = co_await service.update(kind, currentUserId(req), id, body);
    callback(jsonResponse(drogon::k200OK, toJson(kind, entry)));
  } catch (...) {
    callback(http::errorResponseFor(std::current_exception()));
  }
}

drogon::Task<> removeEntry(ReferenceKind kind, drogon::HttpRequestPtr req, Callback callback, int64_t id) {
  ReferenceDataService service(drogon::app().getDbClient());
  try {
    co_await service.remove(kind, currentUserId(req), id);
    callback(jsonResponse(drogon::k200OK, {{"status", "ok"}}));
  } catch (...) {
    callback(http::errorResponseFor(std::current_exception()));
  }
}

} // namespace

drogon::Task<> ReferenceDataController::listCategories(drogon::HttpRequestPtr req, Callback callback) {
  co_await listEntries(ReferenceKind::Category, std::move(req), std::move(callback));
}
drogon::Task<> ReferenceDataController::createCategory(drogon::HttpRequestPtr req, Callback callback) {
  co_await createEntry(ReferenceKind::Category, std::move(req), std::move(callback));
}
drogon::Task<> ReferenceDataController::updateCategory(drogon::HttpRequestPtr req, Callback callback, int64_t id) {
  co_await updateEntry(ReferenceKind::Category, std::move(req), std::move(callback), id);
}
drogon::Task<> ReferenceDataController::removeCategory(drogon::HttpRequestPtr req, Callback callback, int64_t id) {
  co_await removeEntry(ReferenceKind::Category, std::move(req), std::move(callback), id);
}

drogon::Task<> ReferenceDataController::listTags(drogon::HttpRequestPtr req, Callback callback) {
  co_await listEntries(ReferenceKind::Tag, std::move(req), std::move(callback));
}
drogon::Task<> ReferenceDataController::createTag(drogon::HttpRequestPtr req, Callback callback) {
  co_await createEntry(ReferenceKind::Tag, std::move(req), std::move(callback));
}
drogon::Task<> ReferenceDataController::updateTag(drogon::HttpRequestPtr req, Callback callback, int64_t id) {
  co_await updateEntry(ReferenceKind::Tag, std::move(req), std::move(callback), id);
}
drogon::Task<> ReferenceDataController::removeTag(drogon::HttpRequestPtr req, Callback callback, int64_t id) {
  co_await removeEntry(ReferenceKind::Tag, std::move(req), std::move(callback), id);
}

drogon::Task<> ReferenceDataController::listPaymentMethods(drogon::HttpRequestPtr req, Callback callback) {
  co_await listEntries(ReferenceKind::PaymentMethod, std::move(req), std::move(callback));
}
drogon::Task<> ReferenceDataController::createPaymentMethod(drogon::HttpRequestPtr req, Callback callback) {
  co_await createEntry(ReferenceKind::PaymentMethod, std::move(req), std::move(callback));
}
drogon::Task<> ReferenceDataController::updatePaymentMethod(drogon::HttpRequestPtr req, Callback callback,
                                                            int64_t id) {
  co_await updateEntry(ReferenceKind::PaymentMethod, std::move(req), std::move(callback), id);
}
drogon::Task<> ReferenceDataController::removePaymentMethod(drogon::HttpRequestPtr req, Callback callback,
                                                            int64_t id) {
  co_await removeEntry(ReferenceKind::PaymentMethod, std::move(req), std::move(callback), id);
}

} // namespace receipt_scanner
