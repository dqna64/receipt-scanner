#pragma once

#include <array>
#include <string_view>

namespace receipt_scanner::seeds {

// Per-user template constants applied at registration (spec Auth, decided: registration
// seeds THAT user's own categories + "cash" payment method -- NOT global migration rows,
// because tenancy is real multi-user; each tenant owns and can edit its own copy).

inline constexpr std::array<std::string_view, 18> kExpenseCategories = {
    "groceries",      "dining/takeaway", "transport", "fuel",         "household", "utilities",
    "subscriptions",  "insurance",       "health",    "clothing",     "entertainment", "gifts",
    "fees",           "bonds/deposits",  "rewards",   "work-expenses", "junk food", "other",
};

inline constexpr std::array<std::string_view, 4> kIncomeCategories = {
    "salary",
    "interest",
    "dividends/distributions",
    "other-income",
};

inline constexpr std::string_view kDefaultPaymentMethod = "cash";

} // namespace receipt_scanner::seeds
