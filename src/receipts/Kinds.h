#pragma once

#include <array>
#include <optional>
#include <string>
#include <string_view>

// The transaction-kind and direction domains (spec Transaction kinds & stats rules). The single
// C++ source for them: every kind check, default and validation message in src/ derives from
// these tables. Postgres keeps its own copy in migration 001's CHECK constraints
// (receipts_kind_check, receipts_direction_check, chk_direction_by_kind,
// chk_tax_deductible_purchase_only); SQL can't be generated from here, so ledger_test.cpp's
// "kinds:" test fails if the two drift apart. Which kinds count as spend/income in stats is
// deliberately NOT here -- that membership is single-sourced in the SQL views.
namespace receipt_scanner::receipts {

struct KindRules {
  std::string_view name;
  std::optional<std::string_view> canonicalDirection; // nullopt: set freely (a transfer goes either way)
  bool allowsTaxDeductible;                            // decided 2b
  bool canOffset;                                      // may set offsets_receipt_id...
  bool isOffsetTarget;                                 // ...pointing at a receipt of this kind
  bool usesIncomeCategories;                           // else expense categories (spec Review)
};

inline constexpr std::array<KindRules, 5> kKinds{{
    {.name = "purchase", .canonicalDirection = "outflow", .allowsTaxDeductible = true, .canOffset = false,
     .isOffsetTarget = true, .usesIncomeCategories = false},
    {.name = "refund", .canonicalDirection = "inflow", .allowsTaxDeductible = false, .canOffset = true,
     .isOffsetTarget = false, .usesIncomeCategories = false},
    {.name = "reimbursement", .canonicalDirection = "inflow", .allowsTaxDeductible = false, .canOffset = true,
     .isOffsetTarget = false, .usesIncomeCategories = false},
    {.name = "income", .canonicalDirection = "inflow", .allowsTaxDeductible = false, .canOffset = false,
     .isOffsetTarget = false, .usesIncomeCategories = true},
    {.name = "transfer", .canonicalDirection = std::nullopt, .allowsTaxDeductible = false, .canOffset = false,
     .isOffsetTarget = false, .usesIncomeCategories = false},
}};

inline constexpr std::array<std::string_view, 2> kDirections{"inflow", "outflow"};

// Spec: "receipts.kind: purchase (default)" -- manual entry's default, and an upload's
// provisional kind until Step 10's scanner classifies the document.
inline constexpr std::string_view kDefaultKind = "purchase";

// nullptr if `kind` isn't in kKinds.
constexpr const KindRules *findKind(std::string_view kind) {
  for (const auto &rules : kKinds) {
    if (rules.name == kind) {
      return &rules;
    }
  }
  return nullptr;
}

// For a kind already known to be valid (read back from the DB, or validated earlier); throws
// std::logic_error otherwise, since reaching that means a bug, not bad input.
const KindRules &rulesFor(std::string_view kind);

bool isKnownDirection(std::string_view direction);

// "purchase, refund, reimbursement, income, transfer" -- the allowed values in error messages.
std::string kindNames();

// The kinds whose items use income categories as a SQL literal list, e.g. "('income')". Built
// from kKinds (trusted constants, never user input), so it's safe to splice into a query.
std::string incomeKindsSqlList();

} // namespace receipt_scanner::receipts
