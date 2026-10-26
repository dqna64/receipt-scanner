#pragma once

#include "notify/Notifier.h"
#include "scan/MerchantMatcher.h"
#include "scan/ScanProvider.h"
#include "storage/ImageStore.h"

#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>

#include <cstdint>
#include <string>
#include <vector>

namespace receipt_scanner::scan {

struct ScanWorkerConfig {
  int concurrency = 2;                 // receipts in scanning at once, across all workers (spec worker defaults)
  int maxScanAttempts = 3;             // provider/parse errors before scan_failed
  int maxReclaims = 10;                // timeouts/crashes/infra errors before scan_failed (the "high backstop")
  int reclaimAfterSeconds = 600;       // a scan running longer than this is presumed dead
  int matchDateDays = 3;               // verify mode: max |scanned - recorded| purchase date (shared with dedupe)
  double merchantMatchThreshold = 0.3; // verify mode: min MerchantMatcher similarity (pg_trgm's own default)

  // SCAN_CONCURRENCY, SCAN_MAX_ATTEMPTS, SCAN_MAX_RECLAIMS, SCAN_RECLAIM_SECONDS,
  // MATCH_DATE_DAYS, MERCHANT_MATCH_THRESHOLD -- each falling back to the defaults above.
  static ScanWorkerConfig fromEnv();
};

// The background scan pipeline (spec Scan pipeline + Invariants). Postgres is the queue: rows
// in scan_state=uploaded are claimed atomically (FOR UPDATE SKIP LOCKED), so the worker holds no
// state that a restart could lose -- an in-flight scan that dies is simply reclaimed later.
//
// Per claimed receipt, the mode is chosen by TRUST at write time: reviewed_at NULL -> extraction
// (the scan's fields and items replace the row's); reviewed_at set -> verify (fill empty fields
// only, compare the rest; all match -> stays reviewed, any mismatch -> needs_review). Either way
// raw_ocr_json is replaced with the latest scan, which is where the review screen reads the
// scanned-vs-recorded diff from.
class ScanWorker {
public:
  ScanWorker(drogon::orm::DbClientPtr db, storage::ImageStore &imageStore, ScanProvider &provider,
             MerchantMatcher &merchantMatcher, notify::Notifier &notifier, ScanWorkerConfig config);

  // One poll: reclaim scans stuck past reclaimAfterSeconds, then claim up to
  // (concurrency - rows in scanning) uploaded receipts, oldest first, and scan them concurrently.
  // Completes when all of its scans do; the production timer doesn't wait for that, so a tick
  // with a hung scan never stops later ticks. Never throws. Returns the number of receipts
  // claimed. The production timer calls this every SCAN_POLL_SECONDS; tests call it directly.
  drogon::Task<int> tick();

  struct Claim {
    int64_t receiptId;
    int64_t userId;
    // scan_started_at as written by the claim. Every later write requires it to be unchanged,
    // so a scan whose row was re-queued (and possibly claimed again by a later tick) meanwhile
    // can't apply a stale result.
    std::string claimedAt;
  };

private:
  drogon::Task<> reclaimStuck();
  drogon::Task<std::vector<Claim>> claim();
  drogon::Task<> process(Claim claim);
  drogon::Task<> applyResult(const Claim &claim, const ScanResult &result);
  drogon::Task<> recordFailure(const Claim &claim, bool scanError, bool terminal, const std::string &error);
  drogon::Task<> notifyScanFailed(int64_t userId, int64_t receiptId, const std::string &reason);

  drogon::orm::DbClientPtr db_;
  storage::ImageStore &imageStore_;
  ScanProvider &provider_;
  MerchantMatcher &merchantMatcher_;
  notify::Notifier &notifier_;
  ScanWorkerConfig config_;
};

} // namespace receipt_scanner::scan
