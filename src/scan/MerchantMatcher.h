#pragma once

#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>

#include <string>

namespace receipt_scanner::scan {

// Fuzzy merchant comparison: one interface so the algorithm can be swapped (a different
// metric, an LLM judge) without touching callers. Used by verify-mode scans and dedupe.
class MerchantMatcher {
public:
  virtual ~MerchantMatcher() = default;

  // 0 (unrelated) .. 1 (identical).
  virtual drogon::Task<double> similarity(std::string a, std::string b) = 0;
};

// The first implementation: Postgres pg_trgm similarity() (trigram overlap, case-insensitive).
class PgTrgmMerchantMatcher : public MerchantMatcher {
public:
  explicit PgTrgmMerchantMatcher(drogon::orm::DbClientPtr db) : db_(std::move(db)) {}

  drogon::Task<double> similarity(std::string a, std::string b) override;

private:
  drogon::orm::DbClientPtr db_;
};

} // namespace receipt_scanner::scan
