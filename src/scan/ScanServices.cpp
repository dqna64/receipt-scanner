#include "ScanServices.h"

#include "config/Env.h"
#include "notify/Notifier.h"
#include "scan/AnthropicScanProvider.h"
#include "receipts/AppServices.h"
#include "scan/FakeScanProvider.h"
#include "scan/MerchantMatcher.h"

#include <drogon/drogon.h>

#include <stdexcept>

namespace receipt_scanner::scan {

namespace {

// A call that outlives the reclaim window gets its receipt re-queued and scanned a second time
// while the first call is still running. Worst case: every attempt runs to the timeout, plus up
// to 30s of backoff per retry.
AnthropicScanProvider::Config checkedAnthropicConfig() {
  auto config = AnthropicScanProvider::Config::fromEnv();
  // Every attempt plus up to 30s of backoff each, then the rescan's single attempt.
  const double worstCase = (config.timeoutSeconds + 30) * (config.maxRetries + 1) +
                           (config.rescanEffort.empty() ? 0 : config.timeoutSeconds);
  if (worstCase >= ScanWorkerConfig::fromEnv().reclaimAfterSeconds) {
    throw std::runtime_error("SCAN_TIMEOUT_SECONDS is too long for SCAN_RECLAIM_SECONDS: a slow call would be "
                             "re-queued and scanned twice");
  }
  return config;
}

} // namespace

ScanProvider &scanProvider() {
  const auto name = config::envOr("SCAN_PROVIDER", "fake");
  if (name == "fake") {
    static FakeScanProvider instance;
    return instance;
  }
  if (name == "anthropic") {
    static AnthropicScanProvider instance(checkedAnthropicConfig(), drogon::app().getLoop());
    return instance;
  }
  throw std::runtime_error("unknown SCAN_PROVIDER: " + name);
}

ScanWorker &scanWorker() {
  static PgTrgmMerchantMatcher matcher(drogon::app().getDbClient());
  static notify::NoopNotifier notifier;
  static ScanWorker worker(drogon::app().getDbClient(), receipts::imageStore(), scanProvider(), matcher, notifier,
                           ScanWorkerConfig::fromEnv());
  return worker;
}

void startScanWorker() {
  const double pollSeconds = std::stod(config::envOr("SCAN_POLL_SECONDS", "5"));
  auto &worker = scanWorker(); // constructed now, so a bad SCAN_PROVIDER fails at startup
  drogon::app().getLoop()->runEvery(pollSeconds, [&worker] {
    drogon::async_run([&worker]() -> drogon::Task<> { co_await worker.tick(); });
  });
  LOG_INFO << "scan worker polling every " << pollSeconds << "s";
}

} // namespace receipt_scanner::scan
