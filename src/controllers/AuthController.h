#pragma once

#include <drogon/HttpController.h>
#include <drogon/utils/coroutine.h>

namespace receipt_scanner {

class AuthController : public drogon::HttpController<AuthController> {
public:
  METHOD_LIST_BEGIN
  ADD_METHOD_TO(AuthController::registerUser, "/api/v1/auth/register", drogon::Post);
  ADD_METHOD_TO(AuthController::login, "/api/v1/auth/login", drogon::Post);
  ADD_METHOD_TO(AuthController::logout, "/api/v1/auth/logout", drogon::Post,
                "receipt_scanner::filters::SessionAuthFilter");
  ADD_METHOD_TO(AuthController::verifyEmail, "/api/v1/auth/verify", drogon::Post);
  ADD_METHOD_TO(AuthController::requestPasswordReset, "/api/v1/auth/password-reset/request", drogon::Post);
  ADD_METHOD_TO(AuthController::confirmPasswordReset, "/api/v1/auth/password-reset/confirm", drogon::Post);
  METHOD_LIST_END

  drogon::Task<> registerUser(drogon::HttpRequestPtr req, std::function<void(const drogon::HttpResponsePtr &)> callback);
  drogon::Task<> login(drogon::HttpRequestPtr req, std::function<void(const drogon::HttpResponsePtr &)> callback);
  drogon::Task<> logout(drogon::HttpRequestPtr req, std::function<void(const drogon::HttpResponsePtr &)> callback);
  drogon::Task<> verifyEmail(drogon::HttpRequestPtr req, std::function<void(const drogon::HttpResponsePtr &)> callback);
  drogon::Task<> requestPasswordReset(drogon::HttpRequestPtr req,
                                       std::function<void(const drogon::HttpResponsePtr &)> callback);
  drogon::Task<> confirmPasswordReset(drogon::HttpRequestPtr req,
                                       std::function<void(const drogon::HttpResponsePtr &)> callback);
};

} // namespace receipt_scanner
