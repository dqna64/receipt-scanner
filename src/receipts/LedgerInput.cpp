#include "LedgerInput.h"

#include "receipts/Errors.h"
#include "receipts/JsonInput.h"

#include <cmath>

namespace receipt_scanner::receipts {

namespace {

namespace in = json_input;

// A "kind" value that names one of kKinds.
std::string parseKind(const nlohmann::json &value) {
  auto kind = in::requiredString(value, "kind");
  if (!findKind(kind)) {
    throw ValidationError("kind must be one of " + kindNames());
  }
  return kind;
}

// A "direction" value that names one of kDirections.
std::string parseDirection(const nlohmann::json &value) {
  auto direction = in::requiredString(value, "direction");
  if (!isKnownDirection(direction)) {
    throw ValidationError("direction must be inflow or outflow");
  }
  return direction;
}

// Shared by manual entry and PATCH. Kind/direction are handled by the callers, which differ on
// what an absent direction means.
void applyCommonReceiptFields(EditableReceipt &r, const nlohmann::json &body) {
  in::setIfPresent(body, "total_cents", r.totalCents, in::cents);
  in::setIfPresent(body, "currency", r.currency, in::currency);
  in::setIfPresent(body, "merchant", r.merchant, in::nullableString);
  in::setIfPresent(body, "purchase_date", r.purchaseDate, in::nullableDate);
  in::setIfPresent(body, "payment_method_id", r.paymentMethodId, in::nullableId);
  in::setIfPresent(body, "tax_deductible", r.taxDeductible, in::boolean);
  in::setIfPresent(body, "note", r.note, in::nullableString);
  in::setIfPresent(body, "offsets_receipt_id", r.offsetsReceiptId, in::nullableId);
  in::setIfPresent(body, "fx_rate_to_aud", r.fxRateToAud, in::nullablePositiveNumber);
}

// The item counterpart of applyCommonReceiptFields: copies each present key onto `item`.
void applyItemFields(ItemInput &item, const nlohmann::json &body) {
  in::setIfPresent(body, "name", item.name, in::nullableString);
  in::setIfPresent(body, "quantity", item.quantity, in::nullablePositiveNumber);
  in::setIfPresent(body, "unit_price_cents", item.unitPriceCents, in::nullableCents);
  in::setIfPresent(body, "amount_cents", item.amountCents, in::cents);
  in::setIfPresent(body, "category_id", item.categoryId, in::nullableId);
  in::setIfPresent(body, "informational", item.informational, in::boolean);
  in::setIfPresent(body, "tag_ids", item.tagIds, in::idList);
}

// Shared by item create and patch, which accept the same keys.
void rejectUnknownItemKeys(const nlohmann::json &body) {
  in::rejectUnknownKeys(body, {"name", "quantity", "unit_price_cents", "amount_cents", "category_id", "informational",
                               "tag_ids"});
}

} // namespace

ManualEntryInput parseManualEntry(const nlohmann::json &body) {
  in::requireObject(body);
  in::rejectUnknownKeys(body, {"kind", "direction", "total_cents", "currency", "merchant", "purchase_date",
                               "payment_method_id", "tax_deductible", "note", "offsets_receipt_id", "fx_rate_to_aud",
                               "category_id", "items"});
  if (!body.contains("total_cents")) {
    throw ValidationError("total_cents is required");
  }

  ManualEntryInput input;
  auto &r = input.receipt;
  if (body.contains("kind")) r.kind = parseKind(body["kind"]);
  applyCommonReceiptFields(r, body);

  if (body.contains("direction")) {
    r.direction = parseDirection(body["direction"]);
  } else if (auto canonical = rulesFor(r.kind).canonicalDirection) {
    r.direction = *canonical;
  } else {
    throw ValidationError("direction is required when kind is " + r.kind);
  }

  in::setIfPresent(body, "category_id", input.categoryId, in::nullableId);
  if (body.contains("items")) {
    if (!body["items"].is_array()) {
      throw ValidationError("items must be an array");
    }
    if (input.categoryId) {
      throw ValidationError("send either category_id (one implicit item) or items, not both");
    }
    for (const auto &item : body["items"]) {
      input.items.push_back(parseNewItem(item));
    }
  }

  validateReceipt(r);
  return input;
}

void applyReceiptPatch(EditableReceipt &r, const nlohmann::json &body, bool &markReviewed) {
  in::requireObject(body);
  in::rejectUnknownKeys(body, {"kind", "direction", "total_cents", "currency", "merchant", "store_name_raw",
                               "purchase_date", "payment_method_id", "tax_deductible", "note", "offsets_receipt_id",
                               "fx_rate_to_aud", "reviewed"});

  if (body.contains("kind")) {
    r.kind = parseKind(body["kind"]);
    if (!body.contains("direction")) {
      if (auto canonical = rulesFor(r.kind).canonicalDirection) {
        r.direction = *canonical;
      }
    }
  }
  if (body.contains("direction")) r.direction = parseDirection(body["direction"]);
  in::setIfPresent(body, "store_name_raw", r.storeNameRaw, in::nullableString);
  applyCommonReceiptFields(r, body);

  markReviewed = false;
  if (body.contains("reviewed")) {
    if (!in::boolean(body["reviewed"], "reviewed")) {
      throw ValidationError("reviewed only accepts true");
    }
    markReviewed = true;
  }

  validateReceipt(r);
}

ItemInput parseNewItem(const nlohmann::json &body) {
  in::requireObject(body);
  rejectUnknownItemKeys(body);
  if (!body.contains("amount_cents")) {
    throw ValidationError("amount_cents is required");
  }
  ItemInput item;
  applyItemFields(item, body);
  return item;
}

void applyItemPatch(ItemInput &item, const nlohmann::json &body, bool &tagsGiven) {
  in::requireObject(body);
  rejectUnknownItemKeys(body);
  applyItemFields(item, body);
  tagsGiven = body.contains("tag_ids");
}

void validateReceipt(const EditableReceipt &r) {
  const auto &rules = rulesFor(r.kind);
  if (rules.canonicalDirection && *rules.canonicalDirection != r.direction) {
    throw ValidationError("direction must be " + std::string(*rules.canonicalDirection) + " when kind is " + r.kind);
  }
  if (r.taxDeductible && !rules.allowsTaxDeductible) {
    throw ValidationError("tax_deductible is not allowed when kind is " + r.kind);
  }
  if (r.offsetsReceiptId && !rules.canOffset) {
    throw ValidationError("offsets_receipt_id is not allowed when kind is " + r.kind);
  }
  if (r.fxRateToAud && r.currency == "AUD") {
    throw ValidationError("fx_rate_to_aud only applies to non-AUD currencies");
  }
}

bool categoryFitsKind(bool categoryIsIncome, const std::string &kind) {
  return categoryIsIncome == rulesFor(kind).usesIncomeCategories;
}

std::optional<int64_t> audEquivalentCents(int64_t totalCents, const std::optional<double> &fxRateToAud) {
  if (!fxRateToAud) {
    return std::nullopt;
  }
  return std::llround(static_cast<double>(totalCents) * *fxRateToAud);
}

} // namespace receipt_scanner::receipts
