#include "AuthController.h"

#include "auth/AuthService.h"
#include "auth/RateLimiter.h"
#include "config/Env.h"

#include <drogon/drogon.h>
#include <nlohmann/json.hpp>

namespace receipt_scanner {

namespace {

using receipt_scanner::config::envOr;

nlohmann::json parseBody(const drogon::HttpRequestPtr &req) {
  return nlohmann::json::parse(req->body(), nullptr, /*allow_exceptions=*/false);
}

drogon::HttpResponsePtr errorResponse(drogon::HttpStatusCode status, const std::string &code, const std::string &message) {
  nlohmann::json body{{"error", {{"code", code}, {"message", message}}}};
  auto resp = drogon::HttpResponse::newHttpResponse();
  resp->setStatusCode(status);
  resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
  resp->setBody(body.dump());
  return resp;
}

drogon::HttpResponsePtr jsonResponse(drogon::HttpStatusCode status, const nlohmann::json &body) {
  auto resp = drogon::HttpResponse::newHttpResponse();
  resp->setStatusCode(status);
  resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
  resp->setBody(body.dump());
  return resp;
}

// Per-IP AND per-account (decided 3b): per-IP alone is porous behind shared/CGNAT networks,
// per-account alone doesn't stop a credential-stuffing sweep across many emails from one
// source. Process-lifetime in-memory limiters (no Redis at this scale, spec Architecture) --
// function-local statics so login/register each get their own independent budget.
bool checkRateLimit(const std::string &ip, const std::string &email, auth::RateLimiter &byIp,
                     auth::RateLimiter &byAccount) {
  // Always record both attempts (short-circuiting would let an attacker learn which check
  // tripped), then require both to still be within budget.
  const bool ipOk = byIp.allow(ip);
  const bool accountOk = byAccount.allow(email);
  return ipOk && accountOk;
}

auth::RateLimiter &loginIpLimiter() {
  static auth::RateLimiter limiter(std::stoi(envOr("RATE_LIMIT_MAX_ATTEMPTS", "10")),
                                    std::chrono::seconds(std::stoll(envOr("RATE_LIMIT_WINDOW_SECONDS", "300"))));
  return limiter;
}
auth::RateLimiter &loginAccountLimiter() {
  static auth::RateLimiter limiter(std::stoi(envOr("RATE_LIMIT_MAX_ATTEMPTS", "10")),
                                    std::chrono::seconds(std::stoll(envOr("RATE_LIMIT_WINDOW_SECONDS", "300"))));
  return limiter;
}
auth::RateLimiter &registerIpLimiter() {
  static auth::RateLimiter limiter(std::stoi(envOr("RATE_LIMIT_MAX_ATTEMPTS", "10")),
                                    std::chrono::seconds(std::stoll(envOr("RATE_LIMIT_WINDOW_SECONDS", "300"))));
  return limiter;
}
auth::RateLimiter &registerAccountLimiter() {
  static auth::RateLimiter limiter(std::stoi(envOr("RATE_LIMIT_MAX_ATTEMPTS", "10")),
                                    std::chrono::seconds(std::stoll(envOr("RATE_LIMIT_WINDOW_SECONDS", "300"))));
  return limiter;
}

drogon::Cookie makeSessionCookie(const std::string &rawToken) {
  drogon::Cookie cookie("session", rawToken);
  cookie.setHttpOnly(true);
  // Gated by config, not hardcoded true: local dev/CI run over plain HTTP (TLS is Step 18),
  // and a Secure cookie is silently dropped/withheld by browsers AND curl over http://.
  cookie.setSecure(envOr("COOKIE_SECURE", "false") == "true");
  cookie.setSameSite(drogon::Cookie::SameSite::kLax);
  cookie.setPath("/");
  cookie.setMaxAge(static_cast<int>(auth::AuthService::kSessionLifetimeSeconds));
  return cookie;
}

bool sendVerificationEmails() { return envOr("EMAIL_ENABLED", "false") == "true"; }

} // namespace

drogon::Task<> AuthController::registerUser(drogon::HttpRequestPtr req,
                                             std::function<void(const drogon::HttpResponsePtr &)> callback) {
  auto body = parseBody(req);
  if (body.is_discarded() || !body.contains("email") || !body.contains("password")) {
    callback(errorResponse(drogon::k400BadRequest, "invalid_request", "email and password are required"));
    co_return;
  }
  auto email = body["email"].get<std::string>();
  auto password = body["password"].get<std::string>();

  if (!checkRateLimit(req->getPeerAddr().toIp(), email, registerIpLimiter(), registerAccountLimiter())) {
    callback(errorResponse(drogon::k429TooManyRequests, "rate_limited", "too many registration attempts"));
    co_return;
  }

  auth::AuthService authService(drogon::app().getDbClient());
  try {
    auto registered = co_await authService.registerUser(email, password);

    // Scaffolded per spec (Step 5): the token flow exists regardless of the still-open
    // email-infra decision; only the actual SEND is config-gated.
    auto verifyToken = co_await authService.requestEmailVerification(registered.userId);
    if (sendVerificationEmails()) {
      // No provider decided yet (spec Open Eng Decisions) -- nothing to call here until one is.
      LOG_WARN << "EMAIL_ENABLED=true but no email provider is wired up yet; verification email NOT sent";
    } else {
      LOG_INFO << "Email verification token for user " << registered.userId << ": " << verifyToken
                << " (EMAIL_ENABLED=false, logged instead of sent)";
    }

    callback(jsonResponse(drogon::k201Created, {{"id", registered.userId}}));
  } catch (const auth::DuplicateEmailError &) {
    callback(errorResponse(drogon::k409Conflict, "email_taken", "an account with this email already exists"));
  }
}

drogon::Task<> AuthController::login(drogon::HttpRequestPtr req,
                                      std::function<void(const drogon::HttpResponsePtr &)> callback) {
  auto body = parseBody(req);
  if (body.is_discarded() || !body.contains("email") || !body.contains("password")) {
    callback(errorResponse(drogon::k400BadRequest, "invalid_request", "email and password are required"));
    co_return;
  }
  auto email = body["email"].get<std::string>();
  auto password = body["password"].get<std::string>();

  if (!checkRateLimit(req->getPeerAddr().toIp(), email, loginIpLimiter(), loginAccountLimiter())) {
    callback(errorResponse(drogon::k429TooManyRequests, "rate_limited", "too many login attempts"));
    co_return;
  }

  auth::AuthService authService(drogon::app().getDbClient());
  auto session = co_await authService.login(email, password);
  if (!session.has_value()) {
    callback(errorResponse(drogon::k401Unauthorized, "invalid_credentials", "wrong email or password"));
    co_return;
  }

  auto resp = jsonResponse(drogon::k200OK, {{"status", "ok"}});
  resp->addCookie(makeSessionCookie(session->rawToken));
  callback(resp);
}

drogon::Task<> AuthController::logout(drogon::HttpRequestPtr req,
                                       std::function<void(const drogon::HttpResponsePtr &)> callback) {
  const auto &token = req->getCookie("session");
  auth::AuthService authService(drogon::app().getDbClient());
  co_await authService.logout(token);

  auto resp = jsonResponse(drogon::k200OK, {{"status", "ok"}});
  auto expired = drogon::Cookie("session", "");
  expired.setPath("/");
  expired.setMaxAge(0); // clears the cookie client-side
  resp->addCookie(expired);
  callback(resp);
}

drogon::Task<> AuthController::verifyEmail(drogon::HttpRequestPtr req,
                                            std::function<void(const drogon::HttpResponsePtr &)> callback) {
  auto body = parseBody(req);
  if (body.is_discarded() || !body.contains("token")) {
    callback(errorResponse(drogon::k400BadRequest, "invalid_request", "token is required"));
    co_return;
  }

  auth::AuthService authService(drogon::app().getDbClient());
  auto ok = co_await authService.confirmEmailVerification(body["token"].get<std::string>());
  if (!ok) {
    callback(errorResponse(drogon::k400BadRequest, "invalid_token", "verification token is invalid or expired"));
    co_return;
  }
  callback(jsonResponse(drogon::k200OK, {{"status", "ok"}}));
}

drogon::Task<> AuthController::requestPasswordReset(drogon::HttpRequestPtr req,
                                                     std::function<void(const drogon::HttpResponsePtr &)> callback) {
  auto body = parseBody(req);
  if (body.is_discarded() || !body.contains("email")) {
    callback(errorResponse(drogon::k400BadRequest, "invalid_request", "email is required"));
    co_return;
  }
  auto email = body["email"].get<std::string>();

  auth::AuthService authService(drogon::app().getDbClient());
  auto resetToken = co_await authService.requestPasswordReset(email);
  if (sendVerificationEmails()) {
    LOG_WARN << "EMAIL_ENABLED=true but no email provider is wired up yet; reset email NOT sent";
  } else {
    LOG_INFO << "Password reset token for " << email << ": " << resetToken
              << " (EMAIL_ENABLED=false, logged instead of sent)";
  }

  // Always 200, regardless of whether the email existed -- don't leak account existence.
  callback(jsonResponse(drogon::k200OK, {{"status", "ok"}}));
}

drogon::Task<> AuthController::confirmPasswordReset(drogon::HttpRequestPtr req,
                                                     std::function<void(const drogon::HttpResponsePtr &)> callback) {
  auto body = parseBody(req);
  if (body.is_discarded() || !body.contains("token") || !body.contains("password")) {
    callback(errorResponse(drogon::k400BadRequest, "invalid_request", "token and password are required"));
    co_return;
  }

  auth::AuthService authService(drogon::app().getDbClient());
  auto ok = co_await authService.confirmPasswordReset(body["token"].get<std::string>(), body["password"].get<std::string>());
  if (!ok) {
    callback(errorResponse(drogon::k400BadRequest, "invalid_token", "reset token is invalid or expired"));
    co_return;
  }
  callback(jsonResponse(drogon::k200OK, {{"status", "ok"}}));
}

} // namespace receipt_scanner
