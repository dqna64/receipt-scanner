#pragma once

#include <drogon/orm/Row.h>

#include <cstdint>
#include <optional>
#include <string>

// Nullable-column readers shared by the receipts-module services.
namespace receipt_scanner::receipts::row_values {

inline std::optional<std::string> optString(const drogon::orm::Row &row, const std::string &field) {
  return row[field].isNull() ? std::nullopt : std::optional<std::string>(row[field].as<std::string>());
}

inline std::optional<int64_t> optInt(const drogon::orm::Row &row, const std::string &field) {
  return row[field].isNull() ? std::nullopt : std::optional<int64_t>(row[field].as<int64_t>());
}

// NUMERIC columns are selected as ::float8 so this reads a plain double.
inline std::optional<double> optDouble(const drogon::orm::Row &row, const std::string &field) {
  return row[field].isNull() ? std::nullopt : std::optional<double>(row[field].as<double>());
}

} // namespace receipt_scanner::receipts::row_values
