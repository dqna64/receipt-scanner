#pragma once

#include <cstdlib>
#include <string>

namespace receipt_scanner::config {

// Config/env knobs come from the environment, not code, so a fresh clone runs with sane
// defaults and Docker/CI can override per-context (e.g. DB_HOST=postgres in Compose vs
// DB_HOST=localhost for a host-run binary or test against the Compose Postgres's mapped port).
inline std::string envOr(const char *name, std::string fallback) {
  const char *value = std::getenv(name);
  return value != nullptr ? std::string(value) : fallback;
}

} // namespace receipt_scanner::config
