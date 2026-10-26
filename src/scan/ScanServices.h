#pragma once

#include "scan/ScanProvider.h"
#include "scan/ScanWorker.h"

// Lazy singletons for the scan pipeline -- same ambient-lookup pattern (and reasons) as
// receipts::imageStore() in AppServices.h.
namespace receipt_scanner::scan {

// SCAN_PROVIDER selects the adapter; "fake" is currently the only one.
// Throws std::runtime_error for an unknown name, so a typo fails at startup, not mid-scan.
ScanProvider &scanProvider();

ScanWorker &scanWorker();

// Runs scanWorker().tick() every SCAN_POLL_SECONDS (default 5, spec worker defaults) on the main
// loop. Call once, after drogon::app().run() has started the loop (i.e. from a beginning advice).
void startScanWorker();

} // namespace receipt_scanner::scan
