#pragma once

#include <drogon/utils/coroutine.h>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

// The provider boundary (spec HARD REQUIREMENT: trivial provider/model swap). Each adapter owns
// its request shape, prompt, schema and parsing; the rest of the app only sees ScanResult.
namespace receipt_scanner::scan {

// What the provider is told about the receipt and the user's taxonomy. Category and tag names
// come from the requesting user's own lists (spec Invariants, tenant isolation) and become the
// enums in the provider's output schema.
struct ScanContext {
  // Verify mode passes the row's kind (spec: the rescan is "kind-aware, using the row's known
  // kind"); extraction mode leaves it empty so the provider classifies the document.
  std::optional<std::string> knownKind;
  std::vector<std::string> expenseCategories;
  std::vector<std::string> incomeCategories;
  std::vector<std::string> tags;
};

struct ScannedItem {
  std::optional<std::string> name;
  std::optional<double> quantity;
  std::optional<int64_t> unitPriceCents;
  int64_t amountCents = 0;
  std::optional<std::string> category; // a name from ScanContext, or empty
  bool informational = false;          // payslip gross/tax/super lines (spec super boundary)
  std::vector<std::string> tags;       // names from ScanContext
};

// Amounts are *_cents in the spec's unit: 1/100 of the main unit for every currency.
struct ScanResult {
  std::string kind; // purchase | refund | income -- the document types the scanner classifies
  std::string currency = "AUD";
  int64_t totalCents = 0;
  std::optional<std::string> merchant;
  std::optional<std::string> storeNameRaw;
  std::optional<std::string> purchaseDate; // YYYY-MM-DD
  std::vector<ScannedItem> items;
  nlohmann::json raw; // the provider's raw response, stored as receipts.raw_ocr_json
};

// A provider or parse failure: spends one of the receipt's scan_attempts (spec Invariants). Any
// other exception during a scan (S3, Postgres) is infrastructure, not evidence the scan is bad,
// and is budgeted like a reclaim instead.
class ScanError : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

class ScanProvider {
public:
  virtual ~ScanProvider() = default;

  // One call over the receipt's FULL current image set: the provider reconciles
  // overlap and conflicts across images into a single result.
  virtual drogon::Task<ScanResult> scanReceipt(std::vector<std::string> images, ScanContext context) = 0;
};

} // namespace receipt_scanner::scan
