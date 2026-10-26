#pragma once

#include <drogon/utils/coroutine.h>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>

// Notifications go through one interface, swappable per backend (a Discord webhook first,
// email or push later) the same way ScanProvider is per LLM provider.
namespace receipt_scanner::notify {

class Notifier {
public:
  virtual ~Notifier() = default;

  // Best-effort: implementations swallow and log their own delivery failures, so a
  // notification can never fail the work that triggered it.
  virtual drogon::Task<> notify(int64_t userId, std::string event, nlohmann::json payload) = 0;
};

// Used until a real backend exists, and for users with no webhook configured.
class NoopNotifier : public Notifier {
public:
  drogon::Task<> notify(int64_t, std::string, nlohmann::json) override { co_return; }
};

} // namespace receipt_scanner::notify
