#include "FakeScanProvider.h"

#include "receipts/Kinds.h"

namespace receipt_scanner::scan {

namespace {

// The default result: a one-item purchase (or the row's known kind in verify mode).
drogon::Task<ScanResult> cannedResult(const std::vector<std::string> &, const ScanContext &context) {
  ScanResult result;
  result.kind = context.knownKind.value_or(std::string(receipts::kDefaultKind));
  result.merchant = "Fake Merchant";
  result.storeNameRaw = "FAKE MERCHANT PTY LTD";
  result.totalCents = 1234;
  ScannedItem item;
  item.name = "fake item";
  item.amountCents = 1234;
  result.items.push_back(item);
  result.raw = nlohmann::json::object();
  result.raw["provider"] = "fake";
  co_return result;
}

} // namespace

FakeScanProvider::FakeScanProvider() : handler_(cannedResult) {}

FakeScanProvider::FakeScanProvider(Handler handler) : handler_(std::move(handler)) {}

drogon::Task<ScanResult> FakeScanProvider::scanReceipt(std::vector<std::string> images, ScanContext context) {
  {
    std::lock_guard lock(mutex_);
    imageCounts_.push_back(images.size());
  }
  auto result = co_await handler_(images, context);
  co_return result;
}

std::vector<size_t> FakeScanProvider::imageCounts() const {
  std::lock_guard lock(mutex_);
  return imageCounts_;
}

} // namespace receipt_scanner::scan
