#include "ScanServices.h"

#include "config/Env.h"
#include "notify/Notifier.h"
#include "receipts/AppServices.h"
#include "scan/FakeScanProvider.h"
#include "scan/MerchantMatcher.h"

#include <drogon/drogon.h>

#include <stdexcept>

namespace receipt_scanner::scan {

ScanProvider &scanProvider() {
  const auto name = config::envOr("SCAN_PROVIDER", "fake");
  if (name != "fake") {
    throw std::runtime_error("unknown SCAN_PROVIDER: " + name);
  }
  static FakeScanProvider instance;
  return instance;
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
