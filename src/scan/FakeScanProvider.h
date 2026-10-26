#pragma once

#include "scan/ScanProvider.h"

#include <functional>
#include <mutex>

namespace receipt_scanner::scan {

// A stand-in for a real provider: no network, no cost. The default returns a fixed
// result (useful for running the app locally); tests pass a handler to script results or throw
// ScanError, and read back what the worker sent.
class FakeScanProvider : public ScanProvider {
public:
  // A coroutine so a test handler can touch the DB mid-scan (e.g. to simulate a reclaim).
  using Handler = std::function<drogon::Task<ScanResult>(const std::vector<std::string> &images,
                                                         const ScanContext &context)>;

  FakeScanProvider();
  explicit FakeScanProvider(Handler handler);

  drogon::Task<ScanResult> scanReceipt(std::vector<std::string> images, ScanContext context) override;

  // The image count of each call so far, in call order.
  std::vector<size_t> imageCounts() const;

private:
  Handler handler_;
  mutable std::mutex mutex_;
  std::vector<size_t> imageCounts_;
};

} // namespace receipt_scanner::scan
