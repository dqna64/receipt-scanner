#include "SessionAuthFilter.h"

#include "auth/AuthService.h"

#include <drogon/drogon.h>
#include <nlohmann/json.hpp>

namespace receipt_scanner::filters {

namespace {

drogon::HttpResponsePtr unauthorized() {
  nlohmann::json body{{"error", {{"code", "unauthorized"}, {"message", "missing or invalid session"}}}};
  auto resp = drogon::HttpResponse::newHttpResponse();
  resp->setStatusCode(drogon::k401Unauthorized);
  resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
  resp->setBody(body.dump());
  return resp;
}

} // namespace

drogon::Task<drogon::HttpResponsePtr> SessionAuthFilter::doFilter(const drogon::HttpRequestPtr &req) {
  const auto &token = req->getCookie("session");
  if (token.empty()) {
    co_return unauthorized();
  }

  auth::AuthService authService(drogon::app().getDbClient());
  auto userId = co_await authService.validateSession(token);
  if (!userId.has_value()) {
    co_return unauthorized();
  }

  req->attributes()->insert("user_id", *userId);
  co_return nullptr; // nullptr = pass through to the next filter/handler
}

} // namespace receipt_scanner::filters
