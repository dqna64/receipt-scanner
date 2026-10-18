#include "Responses.h"

#include "receipts/Errors.h"

#include <drogon/drogon.h>

namespace receipt_scanner::http {

drogon::HttpResponsePtr errorResponse(drogon::HttpStatusCode status, const std::string &code,
                                      const std::string &message) {
  nlohmann::json body{{"error", {{"code", code}, {"message", message}}}};
  return jsonResponse(status, body);
}

drogon::HttpResponsePtr jsonResponse(drogon::HttpStatusCode status, const nlohmann::json &body) {
  auto resp = drogon::HttpResponse::newHttpResponse();
  resp->setStatusCode(status);
  resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
  resp->setBody(body.dump());
  return resp;
}

nlohmann::json parseBody(const drogon::HttpRequestPtr &req) {
  return nlohmann::json::parse(req->body(), nullptr, /*allow_exceptions=*/false);
}

int64_t currentUserId(const drogon::HttpRequestPtr &req) {
  return req->attributes()->get<int64_t>("user_id");
}

drogon::HttpResponsePtr errorResponseFor(std::exception_ptr error) {
  try {
    std::rethrow_exception(error);
  } catch (const receipts::NotFoundError &e) {
    return errorResponse(drogon::k404NotFound, "not_found", e.what());
  } catch (const receipts::ValidationError &e) {
    return errorResponse(drogon::k400BadRequest, "invalid_request", e.what());
  } catch (const receipts::ConflictError &e) {
    return errorResponse(drogon::k409Conflict, "conflict", e.what());
  } catch (const std::exception &e) {
    LOG_ERROR << "request failed: " << e.what();
  } catch (...) {
    LOG_ERROR << "request failed: non-std exception";
  }
  return errorResponse(drogon::k500InternalServerError, "internal_error", "internal error");
}

} // namespace receipt_scanner::http
