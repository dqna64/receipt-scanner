#include "HealthController.h"

#include <drogon/drogon.h>
#include <nlohmann/json.hpp>

namespace receipt_scanner {

drogon::Task<> HealthController::health(drogon::HttpRequestPtr,
                                         std::function<void(const drogon::HttpResponsePtr &)> callback) {
  nlohmann::json body{{"status", "ok"}};
  auto statusCode = drogon::k200OK;

  try {
    auto db = drogon::app().getDbClient();
    co_await db->execSqlCoro("SELECT 1");
    body["db"] = "ok";
  } catch (const std::exception &e) {
    body["status"] = "degraded";
    body["db"] = "error";
    statusCode = drogon::k503ServiceUnavailable;
  }

  auto resp = drogon::HttpResponse::newHttpResponse();
  resp->setStatusCode(statusCode);
  resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
  resp->setBody(body.dump());
  callback(resp);
  co_return;
}

} // namespace receipt_scanner
