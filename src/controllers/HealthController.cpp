#include "HealthController.h"

#include <nlohmann/json.hpp>

namespace receipt_scanner {

void HealthController::health(const drogon::HttpRequestPtr &, std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
  nlohmann::json body{{"status", "ok"}};
  auto resp = drogon::HttpResponse::newHttpResponse();
  resp->setStatusCode(drogon::k200OK);
  resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
  resp->setBody(body.dump());
  callback(resp);
}

} // namespace receipt_scanner
