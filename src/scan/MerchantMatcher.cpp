#include "MerchantMatcher.h"

namespace receipt_scanner::scan {

drogon::Task<double> PgTrgmMerchantMatcher::similarity(std::string a, std::string b) {
  auto result = co_await db_->execSqlCoro("SELECT similarity($1, $2)::float8 AS s", a, b);
  co_return result[0]["s"].as<double>();
}

} // namespace receipt_scanner::scan
