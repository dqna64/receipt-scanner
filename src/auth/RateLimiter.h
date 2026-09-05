#pragma once

#include <chrono>
#include <mutex>
#include <string>
#include <unordered_map>

namespace receipt_scanner::auth {

// Simple in-memory fixed-window rate limiter (spec, decided 3b: login AND registration are
// rate-limited BOTH per-IP and per-account). No Redis at this scale (spec Architecture) --
// an in-process map is fine for a single-instance deploy; a restart just resets counters,
// which is an acceptable cold-start cost, not a correctness issue.
//
// Two independent limiters are expected to be constructed (one keyed by IP, one by email)
// so a caller can require BOTH to allow before proceeding.
class RateLimiter {
public:
  RateLimiter(int maxAttempts, std::chrono::seconds window) : maxAttempts_(maxAttempts), window_(window) {}

  // Records one attempt for `key` and returns whether it's still within the limit (true =
  // allowed). Called on every attempt, success or failure -- failed logins are exactly the
  // case this is meant to slow down.
  bool allow(const std::string &key) {
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(mutex_);

    auto &entry = attempts_[key];
    if (now - entry.windowStart > window_) {
      entry.windowStart = now;
      entry.count = 0;
    }
    ++entry.count;
    return entry.count <= maxAttempts_;
  }

private:
  struct Entry {
    std::chrono::steady_clock::time_point windowStart = std::chrono::steady_clock::now();
    int count = 0;
  };

  int maxAttempts_;
  std::chrono::seconds window_;
  std::mutex mutex_;
  std::unordered_map<std::string, Entry> attempts_;
};

} // namespace receipt_scanner::auth
