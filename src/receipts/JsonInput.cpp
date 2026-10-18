#include "JsonInput.h"

#include "receipts/Errors.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <limits>

namespace receipt_scanner::receipts::json_input {

namespace {

// Throws "<field> <problem>", e.g. "total_cents must be a non-negative integer (cents)".
[[noreturn]] void fail(std::string_view field, std::string_view problem) {
  throw ValidationError(std::string(field) + " " + std::string(problem));
}

// Strips leading/trailing whitespace.
std::string trim(const std::string &s) {
  auto begin = std::find_if_not(s.begin(), s.end(), [](unsigned char c) { return std::isspace(c); });
  auto end = std::find_if_not(s.rbegin(), s.rend(), [](unsigned char c) { return std::isspace(c); }).base();
  return begin < end ? std::string(begin, end) : std::string();
}

// nlohmann stores a non-negative JSON integer as number_unsigned, which can exceed int64.
std::optional<int64_t> asInt64(const nlohmann::json &value) {
  if (value.is_number_unsigned()) {
    auto u = value.get<uint64_t>();
    if (u > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
      return std::nullopt;
    }
    return static_cast<int64_t>(u);
  }
  if (value.is_number_integer()) {
    return value.get<int64_t>();
  }
  return std::nullopt;
}

} // namespace

void requireObject(const nlohmann::json &body) {
  if (!body.is_object()) {
    throw ValidationError("request body must be a JSON object");
  }
}

void rejectUnknownKeys(const nlohmann::json &body, std::initializer_list<std::string_view> allowed) {
  for (const auto &[key, _] : body.items()) {
    if (std::find(allowed.begin(), allowed.end(), key) == allowed.end()) {
      fail(key, "is not a recognized field");
    }
  }
}

std::string requiredString(const nlohmann::json &value, std::string_view field) {
  if (!value.is_string()) {
    fail(field, "must be a string");
  }
  auto s = trim(value.get<std::string>());
  if (s.empty()) {
    fail(field, "must not be empty");
  }
  return s;
}

std::optional<std::string> nullableString(const nlohmann::json &value, std::string_view field) {
  if (value.is_null()) {
    return std::nullopt;
  }
  if (!value.is_string()) {
    fail(field, "must be a string or null");
  }
  auto s = trim(value.get<std::string>());
  return s.empty() ? std::nullopt : std::optional<std::string>(std::move(s));
}

int64_t cents(const nlohmann::json &value, std::string_view field) {
  auto n = asInt64(value);
  if (!n || *n < 0) {
    fail(field, "must be a non-negative integer (cents)");
  }
  return *n;
}

std::optional<int64_t> nullableCents(const nlohmann::json &value, std::string_view field) {
  return value.is_null() ? std::nullopt : std::optional<int64_t>(cents(value, field));
}

std::optional<int64_t> nullableId(const nlohmann::json &value, std::string_view field) {
  if (value.is_null()) {
    return std::nullopt;
  }
  auto n = asInt64(value);
  if (!n || *n <= 0) {
    fail(field, "must be a positive integer id or null");
  }
  return n;
}

bool boolean(const nlohmann::json &value, std::string_view field) {
  if (!value.is_boolean()) {
    fail(field, "must be true or false");
  }
  return value.get<bool>();
}

std::string date(const nlohmann::json &value, std::string_view field) {
  if (!value.is_string()) {
    fail(field, "must be a YYYY-MM-DD string");
  }
  const auto s = value.get<std::string>();
  const bool shaped = s.size() == 10 && s[4] == '-' && s[7] == '-' &&
                      std::all_of(s.begin(), s.end(), [](char c) { return c == '-' || std::isdigit(static_cast<unsigned char>(c)); });
  if (!shaped) {
    fail(field, "must be a YYYY-MM-DD string");
  }
  const std::chrono::year_month_day ymd{std::chrono::year{std::stoi(s.substr(0, 4))},
                                        std::chrono::month{static_cast<unsigned>(std::stoi(s.substr(5, 2)))},
                                        std::chrono::day{static_cast<unsigned>(std::stoi(s.substr(8, 2)))}};
  if (!ymd.ok()) {
    fail(field, "is not a real calendar date");
  }
  return s;
}

std::optional<std::string> nullableDate(const nlohmann::json &value, std::string_view field) {
  return value.is_null() ? std::nullopt : std::optional<std::string>(date(value, field));
}

std::string currency(const nlohmann::json &value, std::string_view field) {
  if (!value.is_string()) {
    fail(field, "must be a 3-letter currency code");
  }
  auto s = value.get<std::string>();
  if (s.size() != 3 || !std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isalpha(c); })) {
    fail(field, "must be a 3-letter currency code");
  }
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::toupper(c); });
  return s;
}

std::optional<double> nullablePositiveNumber(const nlohmann::json &value, std::string_view field) {
  if (value.is_null()) {
    return std::nullopt;
  }
  if (!value.is_number() || !std::isfinite(value.get<double>()) || value.get<double>() <= 0) {
    fail(field, "must be a positive number or null");
  }
  return value.get<double>();
}

std::vector<int64_t> idList(const nlohmann::json &value, std::string_view field) {
  if (!value.is_array()) {
    fail(field, "must be an array of ids");
  }
  std::vector<int64_t> ids;
  for (const auto &element : value) {
    auto n = asInt64(element);
    if (!n || *n <= 0) {
      fail(field, "must be an array of positive integer ids");
    }
    if (std::find(ids.begin(), ids.end(), *n) == ids.end()) {
      ids.push_back(*n);
    }
  }
  return ids;
}

} // namespace receipt_scanner::receipts::json_input
