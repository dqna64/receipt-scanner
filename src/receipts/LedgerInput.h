#pragma once

#include "receipts/Kinds.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// Request bodies -> validated structs for manual entry, receipt PATCH and item CRUD, plus the
// transaction-kind rules that don't need the DB (spec Transaction kinds & stats rules). Checks
// that need the DB -- ownership of referenced ids, category-vs-kind fit -- live in the services.
// All throw ValidationError. Plain functions, never coroutines (see JsonInput.h).
namespace receipt_scanner::receipts {

// The receipt fields a user can set via manual entry or PATCH.
struct EditableReceipt {
  std::string kind{kDefaultKind};
  std::string direction{*findKind(kDefaultKind)->canonicalDirection};
  int64_t totalCents = 0;
  std::string currency = "AUD";
  std::optional<std::string> merchant;
  std::optional<std::string> storeNameRaw;
  std::optional<std::string> purchaseDate; // nullopt on manual entry -> today in Australia/Sydney
  std::optional<int64_t> paymentMethodId;
  bool taxDeductible = false;
  std::optional<std::string> note;
  std::optional<int64_t> offsetsReceiptId;
  std::optional<double> fxRateToAud;
};

struct ItemInput {
  std::optional<std::string> name;
  std::optional<double> quantity;
  std::optional<int64_t> unitPriceCents;
  int64_t amountCents = 0;
  std::optional<int64_t> categoryId;
  bool informational = false;
  std::vector<int64_t> tagIds;
};

struct ManualEntryInput {
  EditableReceipt receipt;
  // Exactly one of these is used: explicit items, or else ONE implicit item (name=merchant,
  // amount=total, category=categoryId) -- decided 2a, categorization is item-level only, so a
  // manual entry's single "category" field is UI sugar for that implicit item.
  std::optional<int64_t> categoryId;
  std::vector<ItemInput> items;
};

// POST /receipts/manual's body. total_cents is required; kind defaults to kDefaultKind and
// direction to the kind's canonical one (required for transfer).
ManualEntryInput parseManualEntry(const nlohmann::json &body);

// Applies only the keys present in `body`; a JSON null clears a nullable field. Sets
// markReviewed when the body carries "reviewed": true (the only accepted value -- there is no
// un-review). Changing kind without sending direction re-derives it for the fixed kinds and
// keeps the current one for transfer.
void applyReceiptPatch(EditableReceipt &receipt, const nlohmann::json &body, bool &markReviewed);

// An item create body (also each element of manual entry's `items`). amount_cents is required.
ItemInput parseNewItem(const nlohmann::json &body);
// Same partial semantics as applyReceiptPatch; tagsGiven is set when "tag_ids" is present
// (the tag set is replaced wholesale, never merged).
void applyItemPatch(ItemInput &item, const nlohmann::json &body, bool &tagsGiven);

// The DB-free rules on a fully merged receipt: kind/direction consistency, tax_deductible only
// on purchases (decided 2b), offsets only from refund/reimbursement, fx rate only on non-AUD.
void validateReceipt(const EditableReceipt &receipt);

// Whether a category of that side may go on an item of a receipt of `kind` (KindRules::
// usesIncomeCategories; spec Review: the category picker filters by kind).
bool categoryFitsKind(bool categoryIsIncome, const std::string &kind);

// Decided 4a: computed once from the user-entered rate at entry/edit time, never live.
std::optional<int64_t> audEquivalentCents(int64_t totalCents, const std::optional<double> &fxRateToAud);

} // namespace receipt_scanner::receipts
