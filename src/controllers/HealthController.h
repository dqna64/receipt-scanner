#pragma once

#include <drogon/HttpController.h>
#include <drogon/utils/coroutine.h>

namespace receipt_scanner {

class HealthController : public drogon::HttpController<HealthController> {
public:
  METHOD_LIST_BEGIN
  ADD_METHOD_TO(HealthController::health, "/api/v1/health", drogon::Get);
  METHOD_LIST_END

  drogon::Task<> health(drogon::HttpRequestPtr req,
                         std::function<void(const drogon::HttpResponsePtr &)> callback);
};

} // namespace receipt_scanner
