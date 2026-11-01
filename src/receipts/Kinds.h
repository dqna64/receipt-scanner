#pragma once

#include <array>
#include <optional>
#include <string>
#include <string_view>

// The transaction-kind and direction domains (spec Transaction kinds & stats rules). The single
// C++ source for them: every kind check, default and validation message in src/, and the
// scanner prompt's kind descriptions, derive from these tables. Postgres keeps its own copy in
// migration 001's CHECK constraints (receipts_kind_check, receipts_direction_check, chk_direction_by_kind,
// chk_tax_deductible_purchase_only); SQL can't be generated from here, so ledger_test.cpp's
// "kinds:" test fails if the two drift apart. Which kinds count as spend/income in stats is
// deliberately NOT here -- that membership is single-sourced in the SQL views.
namespace receipt_scanner::receipts {

struct KindRules {
  std::string_view name;
  std::optional<std::string_view> canonicalDirection; // nullopt: set freely (a transfer goes either way)
  bool allowsTaxDeductible;                            // only purchases
  bool canOffset;                                      // may set offsets_receipt_id...
  bool isOffsetTarget;                                 // ...pointing at a receipt of this kind
  bool usesIncomeCategories;                           // else expense categories (spec Review)

  // How a scanner recognises and reads this kind, in provider-neutral words that an LLM adapter
  // puts in its prompt. Empty `scanDocuments`: the scanner never classifies a document as this
  // kind (it still verifies rows of it, keeping their own kind).
  std::string_view scanDocuments{}; // what is photographed for this kind
  std::string_view scanTotal{};     // what the total means for it
  std::string_view scanRules{};     // anything else specific to it, or empty

  constexpr bool scannable() const { return !scanDocuments.empty(); }
};

inline constexpr std::array<KindRules, 5> kKinds{{
    {.name = "purchase", .canonicalDirection = "outflow", .allowsTaxDeductible = true, .canOffset = false,
     .isOffsetTarget = true, .usesIncomeCategories = false,
     .scanDocuments = "a receipt, tax invoice or order confirmation for something bought",
     .scanTotal = "the final amount paid, after discounts and including tax, surcharges and tips that are printed"},
    {.name = "refund", .canonicalDirection = "inflow", .allowsTaxDeductible = false, .canOffset = true,
     .isOffsetTarget = false, .usesIncomeCategories = false,
     .scanDocuments = "a return or refund docket from the business that sold something: money back for a return",
     .scanTotal = "the amount refunded"},
    {.name = "reimbursement", .canonicalDirection = "inflow", .allowsTaxDeductible = false, .canOffset = true,
     .isOffsetTarget = false, .usesIncomeCategories = false,
     .scanDocuments = "proof that someone (an employer, a friend) paid the user back for an expense: an expense "
                      "app or claim marked reimbursed or paid, a remittance advice, a payment notification",
     .scanTotal = "the amount actually paid back to the user for the expense shown, in the currency it was "
                  "paid in, which may differ from the expense's own amount (e.g. after currency conversion); if a "
                  "deposit covers several expenses, only this expense's share",
     .scanRules = "merchant is whoever paid the user back (an employer, a friend, a perks program), or null if "
                  "the documents don't name them; not the business where the expense was made. purchase_date is "
                  "the date the money was paid back (null if only the expense date is shown).\n"
                  "- Exactly one item: name \"Reimbursement\", amount equal to the total, category the one that "
                  "best fits the expense being reimbursed. It offsets the whole expense, so do not list the "
                  "expense's own lines."},
    {.name = "income", .canonicalDirection = "inflow", .allowsTaxDeductible = false, .canOffset = false,
     .isOffsetTarget = false, .usesIncomeCategories = true,
     .scanDocuments = "a payslip, pay advice or paycheck",
     .scanTotal = "the NET pay (the take-home amount paid into the bank), never the gross",
     .scanRules = "merchant and store_name_raw are the employer, and purchase_date is the pay date.\n"
                  "- Exactly one item with informational=false: name \"Net pay\", amount equal to the total.\n"
                  "- Optionally, gross pay, tax withheld, superannuation and other deductions as separate items "
                  "with informational=true and category null."},
    {.name = "transfer", .canonicalDirection = std::nullopt, .allowsTaxDeductible = false, .canOffset = false,
     .isOffsetTarget = false, .usesIncomeCategories = false},
}};

inline constexpr std::array<std::string_view, 2> kDirections{"inflow", "outflow"};

// Extraction writes a scanned kind straight to the row and derives the direction from it, so a
// kind the scanner can classify must have a fixed one.
static_assert([] {
  for (const auto &rules : kKinds) {
    if (rules.scannable() && !rules.canonicalDirection) {
      return false;
    }
  }
  return true;
}(), "a scannable kind needs a canonicalDirection");

// Spec: "receipts.kind: purchase (default)" -- manual entry's default, and an upload's
// provisional kind until the scanner classifies the document.
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
