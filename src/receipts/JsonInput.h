#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Typed field readers for request bodies. Each throws receipts::ValidationError with a message
// naming the field. Plain functions, never coroutines: request parsing stays out of coroutine
// bodies so braced-init lists (allowed-key lists, json literals) never sit in a co_await
// expression -- the GCC 12/13 ICE pattern from Step 8.
namespace receipt_scanner::receipts::json_input {

void requireObject(const nlohmann::json &body);
// Typos like "totl_cents" fail loudly instead of being silently ignored.
void rejectUnknownKeys(const nlohmann::json &body, std::initializer_list<std::string_view> allowed);

// Trimmed; empty is an error.
std::string requiredString(const nlohmann::json &value, std::string_view field);
// null, or a string that's empty after trimming -> nullopt.
std::optional<std::string> nullableString(const nlohmann::json &value, std::string_view field);
// Integer >= 0 (spec Money: non-negative magnitude, never a sign). 1/100 of the main unit
// for every currency -- yen too (¥2000 = 200000), whatever ISO 4217 says.
int64_t cents(const nlohmann::json &value, std::string_view field);
std::optional<int64_t> nullableCents(const nlohmann::json &value, std::string_view field);
// Positive integer, or null.
std::optional<int64_t> nullableId(const nlohmann::json &value, std::string_view field);
bool boolean(const nlohmann::json &value, std::string_view field);
// "YYYY-MM-DD" naming a real calendar date (2026-02-30 is rejected here, not by Postgres).
std::string date(const nlohmann::json &value, std::string_view field);
std::optional<std::string> nullableDate(const nlohmann::json &value, std::string_view field);
// Three ASCII letters, returned uppercased.
std::string currency(const nlohmann::json &value, std::string_view field);
// Finite and > 0, or null.
std::optional<double> nullablePositiveNumber(const nlohmann::json &value, std::string_view field);
// Array of positive integers; duplicates dropped, order kept.
std::vector<int64_t> idList(const nlohmann::json &value, std::string_view field);

// If `key` is present in `body`, sets target = read(body[key], key) with one of the readers
// above; otherwise leaves target untouched (PATCH semantics: absent = keep). Exists so each
// field's key is spelled once at the call site -- spelled three times, a typo in one copy
// silently skips the field or mislabels its error.
template <typename T, typename Reader>
void setIfPresent(const nlohmann::json &body, std::string_view key, T &target, Reader read) {
  if (auto it = body.find(std::string(key)); it != body.end()) {
    target = read(*it, key);
  }
}

} // namespace receipt_scanner::receipts::json_input
