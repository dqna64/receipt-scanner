#pragma once

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <exception>
#include <optional>
#include <string>

// Response/request helpers shared by the receipts-module controllers (spec API conventions:
// error envelope {error: {code, message}}).
namespace receipt_scanner::http {

drogon::HttpResponsePtr errorResponse(drogon::HttpStatusCode status, const std::string &code,
                                      const std::string &message);
drogon::HttpResponsePtr jsonResponse(drogon::HttpStatusCode status, const nlohmann::json &body);

// Malformed JSON comes back discarded (not an object), which the services reject as a 400.
nlohmann::json parseBody(const drogon::HttpRequestPtr &req);

// Set by SessionAuthFilter.
int64_t currentUserId(const drogon::HttpRequestPtr &req);

// receipts::NotFoundError -> 404, ValidationError -> 400 (what() is the message),
// ConflictError -> 409; anything else is logged and becomes a generic 500 -- internals never
// reach the client.
drogon::HttpResponsePtr errorResponseFor(std::exception_ptr error);

template <typename T> nlohmann::json nullable(const std::optional<T> &value) {
  return value ? nlohmann::json(*value) : nlohmann::json(nullptr);
}

} // namespace receipt_scanner::http
