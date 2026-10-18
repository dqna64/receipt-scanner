#include "Kinds.h"

#include <algorithm>
#include <stdexcept>

namespace receipt_scanner::receipts {

const KindRules &rulesFor(std::string_view kind) {
  const auto *rules = findKind(kind);
  if (!rules) {
    throw std::logic_error("unknown kind: " + std::string(kind));
  }
  return *rules;
}

bool isKnownDirection(std::string_view direction) {
  return std::find(kDirections.begin(), kDirections.end(), direction) != kDirections.end();
}

std::string kindNames() {
  std::string names;
  for (const auto &rules : kKinds) {
    names += (names.empty() ? "" : ", ") + std::string(rules.name);
  }
  return names;
}

std::string incomeKindsSqlList() {
  std::string list;
  for (const auto &rules : kKinds) {
    if (rules.usesIncomeCategories) {
      list += (list.empty() ? "'" : ", '") + std::string(rules.name) + "'";
    }
  }
  return "(" + list + ")";
}

} // namespace receipt_scanner::receipts
