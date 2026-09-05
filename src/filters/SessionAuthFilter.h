#pragma once

#include <drogon/HttpFilter.h>
#include <drogon/utils/coroutine.h>

namespace receipt_scanner::filters {

// Guards every route it's attached to (spec Auth: "Drogon filter guarding all routes").
// Reads the session cookie, validates it via AuthService (sliding-expiry touch included),
// and attaches the owning user_id to the request's attributes for handlers to read -- the
// user_id predicate this enables downstream is load-bearing tenant isolation, not
// future-proofing (spec Invariants).
class SessionAuthFilter : public drogon::HttpCoroFilter<SessionAuthFilter> {
public:
  drogon::Task<drogon::HttpResponsePtr> doFilter(const drogon::HttpRequestPtr &req) override;
};

} // namespace receipt_scanner::filters
