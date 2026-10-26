#pragma once

#include <array>
#include <string_view>

// The scan_state and source domains, single-sourced like Kinds.h. Postgres keeps its own copy
// in migration 001's receipts_scan_state_check / receipts_source_check; ledger_test.cpp's
// "kinds:" test fails if the two drift apart.
namespace receipt_scanner::receipts {

// The scan-pipeline lane ONLY (spec Invariants, two-axis state) -- review state is derived from
// proposed_at/reviewed_at, never stored here.
namespace scan_state {
inline constexpr std::string_view kIdle = "idle";             // at rest: manual, recurring, or scan complete
inline constexpr std::string_view kUploaded = "uploaded";     // queued for the worker
inline constexpr std::string_view kScanning = "scanning";     // claimed, in flight
inline constexpr std::string_view kScanFailed = "scan_failed"; // terminal but retryable
inline constexpr std::array<std::string_view, 4> kAll{kIdle, kUploaded, kScanning, kScanFailed};
} // namespace scan_state

namespace source {
inline constexpr std::string_view kScanned = "scanned";
inline constexpr std::string_view kManual = "manual";
inline constexpr std::string_view kRecurring = "recurring";
inline constexpr std::array<std::string_view, 3> kAll{kScanned, kManual, kRecurring};
} // namespace source

} // namespace receipt_scanner::receipts
