#include "AnthropicScanProvider.h"

#include <drogon/drogon.h>
#include <drogon/utils/Utilities.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <stdexcept>

#include "config/Env.h"
#include "receipts/Kinds.h"

namespace receipt_scanner::scan {

namespace {

constexpr const char *kApiVersion = "2026-10-21";

std::string kindNamesFor(bool incomeCategories) {
  std::string out;
  for (const auto &rules : receipts::kKinds) {
    if (rules.scannable() && rules.usesIncomeCategories == incomeCategories) {
      out += (out.empty() ? "" : ", ") + std::string(rules.name);
    }
  }
  return out;
}

// Built once and identical on every call; everything per-user goes in the user message.
const std::string &systemPrompt() {
  static const std::string prompt = [] {
    std::string kinds;
    std::string totals;
    std::string extraRules;
    for (const auto &rules : receipts::kKinds) {
      if (!rules.scannable()) {
        continue;
      }
      const std::string kind(rules.name);
      kinds += "- " + kind + ": " + std::string(rules.scanDocuments) + ".\n";
      totals += " For " + kind + ", " + std::string(rules.scanTotal) + ".";
      if (!rules.scanRules.empty()) {
        extraRules += "\nFor " + kind + ":\n- " + std::string(rules.scanRules) + "\n";
      }
    }
    return std::string(
               "You extract structured data from photos of financial documents for a personal spending and income "
               "ledger.\n\n"
               "The user message contains one or more images followed by instructions. All the images belong to ONE "
               "transaction: for example two overlapping photos of one long receipt, or a receipt plus a screenshot "
               "of an email correcting it. Return one result for the transaction.\n\n"
               "Several images:\n"
               "- They may overlap. Count each line item once.\n"
               "- If they conflict, prefer the clearer reading, and prefer a later correction over the figure it "
               "corrects.\n\n"
               "Transaction kind, when you are asked to classify it, by the type of document shown:\n") +
           kinds +
           "\nFields:\n"
           "- total: the amount that actually changed hands, as a positive number in the main currency unit, "
           "exactly as printed (23.50, not 2350)." +
           totals +
           "\n- currency: the ISO 4217 code shown by the symbol, text, address or country (e.g. \"$\" on an "
           "Australian receipt is AUD, \"¥\" in Japan is JPY). Use AUD only when the documents give no signal.\n"
           "- merchant: the short brand name a person would use for the business, e.g. \"Woolworths\", "
           "\"Bunnings\", \"McDonald's\", not the legal entity or branch.\n"
           "- store_name_raw: the business name exactly as printed at the top, including any branch or store "
           "number, e.g. \"WOOLWORTHS METRO 1234 SYDNEY\". Null if no name is printed.\n"
           "- purchase_date: the transaction date as YYYY-MM-DD. Read ambiguous numeric dates day-first "
           "(03/04/2026 is 3 April 2026) unless the document is from a country that writes month-first. Null if "
           "no date is printed.\n"
           "- unreadable_reason: null normally. If the images don't show one of the document types above, or the "
           "total cannot be read (too blurry, cut off), a short reason; still fill in what you can. Items that "
           "can't all be read, or that don't add up to the total (some out of frame), are not a reason: return "
           "the ones you can read.\n\n"
           "Items (the transaction's lines):\n"
           "- amount: the line's total as printed: positive, except a discount or a rounding down, which is "
           "negative.\n"
           "- quantity and unit_price: the numbers printed for the line, or null when the line doesn't print "
           "them.\n"
           "- The same product on several lines at the same price is one item, with the combined quantity and "
           "amount.\n"
           "- A discount on one line reduces that line's amount; it is not an item of its own. A deal across "
           "several lines (such as \"buy 2 for $10\") is not a one-line discount, even when it prints under the "
           "last of them.\n"
           "- Every other line that changes the amount paid is an item: a whole-transaction discount or voucher "
           "(negative), a card, weekend or public holiday surcharge, a service, booking, delivery or shipping fee, "
           "a tip, tax added on top of the prices, and cash rounding. Its category is the category of the items "
           "it applies to (for shipping, the items shipped), not a category of its own such as fees or other; if "
           "those items span several categories, the one with the largest amount.\n"
           "- Lines that don't change the amount paid are not items: subtotal, total, tax already included in "
           "the prices, payment, change, and savings summaries that repeat discounts already taken off the "
           "lines.\n"
           "- A tax line (GST, VAT, sales tax) is either already included in the item prices or added on top of "
           "them, and documents print both the same way: under a subtotal, or even below the total. Work out "
           "which from the document: a note such as \"incl. GST\" or \"GST inclusive items\", a total "
           "labelled as including tax, and whether the lines reach the total without the tax line. List the "
           "tax as an item only when it is added on top.\n"
           "- Check that the item amounts add up to the total. If they don't, re-read them: printed amounts are "
           "not always on the same line as their item.\n"
           "- category: pick from the given categories for the transaction's kind (expense categories for " +
           kindNamesFor(false) + "; income categories for " + kindNamesFor(true) +
           "). Null if none fits.\n"
           "- tags: only from the given tag list, and only when one clearly applies. Usually empty.\n"
           "- informational: false, unless a kind's rules below say otherwise.\n" +
           extraRules +
           "\nNever invent values that are not on the documents, apart from the currency default.";
  }();
  return prompt;
}

std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
  return s;
}

// Stored images are normalized WebP; the others are accepted so the golden-set harness and tests
// can send any format the API takes.
std::string mediaType(const std::string &bytes) {
  if (bytes.size() >= 12 && bytes.compare(0, 4, "RIFF") == 0 && bytes.compare(8, 4, "WEBP") == 0) {
    return "image/webp";
  }
  if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xFF &&
      static_cast<unsigned char>(bytes[1]) == 0xD8) {
    return "image/jpeg";
  }
  if (bytes.size() >= 8 && bytes.compare(0, 8, "\x89PNG\r\n\x1a\n") == 0) {
    return "image/png";
  }
  if (bytes.size() >= 4 && bytes.compare(0, 4, "GIF8") == 0) {
    return "image/gif";
  }
  throw ScanError("image is not WebP, JPEG, PNG or GIF");
}

// Names from both lists, first spelling kept, case-insensitive duplicates dropped (an expense and
// an income category may share a name; the worker resolves it against the kind's side).
std::vector<std::string> uniqueNames(const std::vector<std::vector<std::string>> &lists) {
  std::vector<std::string> out;
  for (const auto &list : lists) {
    for (const auto &name : list) {
      const auto key = lower(name);
      if (std::none_of(out.begin(), out.end(), [&](const std::string &seen) { return lower(seen) == key; })) {
        out.push_back(name);
      }
    }
  }
  return out;
}

nlohmann::json nullable(nlohmann::json schema) {
  nlohmann::json nullSchema = {{"type", "null"}};
  return {{"anyOf", nlohmann::json::array({std::move(schema), nullSchema})}};
}

// A string from `names`, or null; just null when the list is empty (an empty enum is invalid).
nlohmann::json enumOrNull(const std::vector<std::string> &names) {
  if (names.empty()) {
    return {{"type", "null"}};
  }
  nlohmann::json schema = {{"type", "string"}, {"enum", names}};
  return nullable(std::move(schema));
}

nlohmann::json objectSchema(nlohmann::json properties) {
  nlohmann::json required = nlohmann::json::array();
  for (const auto &[key, _] : properties.items()) {
    required.push_back(key);
  }
  return {{"type", "object"},
          {"properties", std::move(properties)},
          {"required", std::move(required)},
          {"additionalProperties", false}};
}

std::vector<std::string> categoriesFor(const ScanContext &context) {
  if (!context.knownKind) {
    return uniqueNames({context.expenseCategories, context.incomeCategories});
  }
  return receipts::rulesFor(*context.knownKind).usesIncomeCategories ? context.incomeCategories
                                                                     : context.expenseCategories;
}

nlohmann::json outputSchema(const ScanContext &context) {
  nlohmann::json item = {
      {"name", nullable({{"type", "string"}})},         {"quantity", nullable({{"type", "number"}})},
      {"unit_price", nullable({{"type", "number"}})},   {"amount", {{"type", "number"}}},
      {"category", enumOrNull(categoriesFor(context))}, {"informational", {{"type", "boolean"}}},
  };
  if (!context.tags.empty()) {
    item["tags"] = {{"type", "array"}, {"items", {{"type", "string"}, {"enum", context.tags}}}};
  }

  nlohmann::json properties = {
      {"unreadable_reason", nullable({{"type", "string"}})},
      {"currency", {{"type", "string"}}},
      {"total", {{"type", "number"}}},
      {"merchant", nullable({{"type", "string"}})},
      {"store_name_raw", nullable({{"type", "string"}})},
      {"purchase_date", nullable({{"type", "string"}, {"format", "date"}})},
      {"items", {{"type", "array"}, {"items", objectSchema(std::move(item))}}},
  };
  if (!context.knownKind) {
    nlohmann::json kinds = nlohmann::json::array();
    for (const auto &rules : receipts::kKinds) {
      if (rules.scannable()) {
        kinds.push_back(rules.name);
      }
    }
    properties["kind"] = {{"type", "string"}, {"enum", std::move(kinds)}};
  }
  return objectSchema(std::move(properties));
}

std::string joined(const std::vector<std::string> &names) {
  if (names.empty()) {
    return "(none)";
  }
  std::string out;
  for (const auto &name : names) {
    out += (out.empty() ? "" : ", ") + name;
  }
  return out;
}

std::string instructions(const ScanContext &context, size_t imageCount) {
  std::string text = imageCount == 1 ? "The image above is one transaction."
                                     : "The " + std::to_string(imageCount) + " images above are one transaction.";
  if (context.knownKind) {
    text += " It is already known to be of kind \"" + *context.knownKind + "\"; extract the fields for that kind.";
  } else {
    text += " Classify its kind, then extract the fields.";
  }
  if (!context.knownKind || !receipts::rulesFor(*context.knownKind).usesIncomeCategories) {
    text += "\n\nExpense categories: " + joined(context.expenseCategories);
  }
  if (!context.knownKind || receipts::rulesFor(*context.knownKind).usesIncomeCategories) {
    text += "\nIncome categories: " + joined(context.incomeCategories);
  }
  text += "\nTags: " + joined(context.tags);
  if (context.scannerNotes.find_first_not_of(" \t\r\n") != std::string::npos) {
    text += "\n\nThe user's own notes about their documents. A note applies only to a document it clearly "
            "describes, and only to the fields it mentions: there it overrides the general rules, and every "
            "other field follows the general rules as usual. Ignore the notes that don't match this "
            "transaction, and never take values from a note that the documents don't show:\n<user_notes>\n" +
            context.scannerNotes + "\n</user_notes>";
  }
  return text;
}

// Main-unit amount as printed -> cents. The model reads "23.50" off the page; doing the x100
// here keeps arithmetic out of the model's job.
int64_t toCents(double amount) {
  return static_cast<int64_t>(std::llround(amount * 100));
}

std::optional<std::string> optText(const nlohmann::json &object, const char *key) {
  auto it = object.find(key);
  if (it == object.end() || !it->is_string()) {
    return std::nullopt;
  }
  auto value = it->get<std::string>();
  if (value.find_first_not_of(" \t\r\n") == std::string::npos) {
    return std::nullopt;
  }
  return value;
}

std::optional<double> optNumber(const nlohmann::json &object, const char *key) {
  auto it = object.find(key);
  if (it == object.end() || !it->is_number()) {
    return std::nullopt;
  }
  return it->get<double>();
}

ScannedItem toItem(const nlohmann::json &line) {
  ScannedItem item;
  item.name = optText(line, "name");
  item.amountCents = toCents(line.at("amount").get<double>());
  if (auto quantity = optNumber(line, "quantity"); quantity && *quantity > 0) {
    item.quantity = quantity;
  }
  if (auto unitPrice = optNumber(line, "unit_price"); unitPrice && *unitPrice >= 0) {
    item.unitPriceCents = toCents(*unitPrice);
  }
  item.category = optText(line, "category");
  item.informational = line.at("informational").get<bool>();
  if (auto tags = line.find("tags"); tags != line.end()) {
    item.tags = tags->get<std::vector<std::string>>();
  }
  return item;
}

std::string apiErrorMessage(const std::string &body) {
  auto parsed = nlohmann::json::parse(body, nullptr, /*allow_exceptions=*/false);
  if (parsed.is_object() && parsed.contains("error") && parsed["error"].is_object()) {
    return parsed["error"].value("type", "") + ": " + parsed["error"].value("message", "");
  }
  return body.substr(0, 300);
}

bool retryable(drogon::HttpStatusCode status) {
  const int code = static_cast<int>(status);
  return code == 408 || code == 429 || code >= 500;
}

}  // namespace

AnthropicScanProvider::Config AnthropicScanProvider::Config::fromEnv() {
  Config config;
  config.apiKey = config::envOr("ANTHROPIC_API_KEY", "");
  if (config.apiKey.empty()) {
    throw std::runtime_error("SCAN_PROVIDER=anthropic needs ANTHROPIC_API_KEY");
  }
  config.model = config::envOr("ANTHROPIC_MODEL", config.model);
  config.baseUrl = config::envOr("ANTHROPIC_BASE_URL", config.baseUrl);
  config.timeoutSeconds = std::stod(config::envOr("SCAN_TIMEOUT_SECONDS", std::to_string(config.timeoutSeconds)));
  config.maxTokens = std::stoi(config::envOr("ANTHROPIC_MAX_TOKENS", std::to_string(config.maxTokens)));
  config.effort = config::envOr("ANTHROPIC_EFFORT", config.effort);
  config.rescanEffort = config::envOr("ANTHROPIC_RESCAN_EFFORT", config.rescanEffort);
  return config;
}

AnthropicScanProvider::AnthropicScanProvider(Config config, trantor::EventLoop *loop)
    : config_(std::move(config)), loop_(loop), client_(drogon::HttpClient::newHttpClient(config_.baseUrl, loop)) {}

nlohmann::json AnthropicScanProvider::buildRequest(const std::vector<std::string> &images, const ScanContext &context,
                                                   const Config &config) {
  nlohmann::json content = nlohmann::json::array();
  for (size_t i = 0; i < images.size(); ++i) {
    if (images.size() > 1) {
      content.push_back({{"type", "text"}, {"text", "Image " + std::to_string(i + 1) + ":"}});
    }
    const auto &bytes = images[i];
    content.push_back({{"type", "image"},
                       {"source",
                        {{"type", "base64"},
                         {"media_type", mediaType(bytes)},
                         {"data", drogon::utils::base64Encode(reinterpret_cast<const unsigned char *>(bytes.data()),
                                                              bytes.size())}}}});
  }
  content.push_back({{"type", "text"}, {"text", instructions(context, images.size())}});

  nlohmann::json message = {{"role", "user"}, {"content", std::move(content)}};
  return {
      {"model", config.model},
      {"max_tokens", config.maxTokens},
      {"system", systemPrompt()},
      {"messages", nlohmann::json::array({std::move(message)})},
      {"output_config",
       {{"effort", config.effort}, {"format", {{"type", "json_schema"}, {"schema", outputSchema(context)}}}}},
  };
}

ScanResult AnthropicScanProvider::parseResponse(const std::string &body, const ScanContext &context) {
  auto response = nlohmann::json::parse(body, nullptr, /*allow_exceptions=*/false);
  if (!response.is_object()) {
    throw ScanError("Anthropic response is not JSON");
  }
  const auto stopReason = response.value("stop_reason", "");
  if (stopReason == "refusal") {
    throw ScanError("model refused to read the document");
  }
  if (stopReason == "max_tokens") {
    throw ScanError("model reply was cut off at max_tokens");
  }

  const nlohmann::json *text = nullptr;
  if (auto content = response.find("content"); content != response.end() && content->is_array()) {
    for (const auto &block : *content) {
      if (block.value("type", "") == "text") {
        text = &block;
        break;
      }
    }
  }
  if (!text) {
    throw ScanError("Anthropic response has no text block (stop_reason " + stopReason + ")");
  }

  ScanResult result;
  try {
    auto output = nlohmann::json::parse(text->at("text").get<std::string>());
    if (auto reason = optText(output, "unreadable_reason")) {
      throw ScanError("model could not read the document: " + *reason);
    }
    result.kind = context.knownKind ? *context.knownKind : output.at("kind").get<std::string>();
    result.currency = output.at("currency").get<std::string>();
    // Magnitudes (spec Money): a refund docket may print its total as negative.
    result.totalCents = std::llabs(toCents(output.at("total").get<double>()));
    result.merchant = optText(output, "merchant");
    result.storeNameRaw = optText(output, "store_name_raw");
    result.purchaseDate = optText(output, "purchase_date");
    for (const auto &line : output.at("items")) {
      auto item = toItem(line);
      // Returned lines on a refund docket are printed negative: keep their magnitude. On any other
      // document a negative line is a whole-transaction discount or rounding, which stays negative
      // so that the items add up to the total and the discount nets against its category.
      if (item.amountCents < 0 && result.kind == "refund") {
        item.amountCents = -item.amountCents;
      }
      // A zero line (a "$0.00 docket deal") moves no money, so it would only clutter review.
      if (item.amountCents == 0) {
        continue;
      }
      result.items.push_back(std::move(item));
    }
    result.raw = {
        {"provider", "anthropic"},
        {"model", response.value("model", "")},
        {"message_id", response.value("id", "")},
        {"usage", response.value("usage", nlohmann::json::object())},
        {"output", std::move(output)},
    };
  } catch (const nlohmann::json::exception &e) {
    throw ScanError(std::string("Anthropic reply doesn't match the extraction schema: ") + e.what());
  }
  return result;
}

bool AnthropicScanProvider::itemsAddUp(const ScanResult &result) {
  int64_t sum = 0;
  for (const auto &item : result.items) {
    if (!item.informational) {
      sum += item.amountCents;
    }
  }
  return result.items.empty() || sum == result.totalCents;
}

drogon::Task<ScanResult> AnthropicScanProvider::scanReceipt(std::vector<std::string> images, ScanContext context) {
  const auto firstBody = co_await send(buildRequest(images, context, config_).dump(), config_.maxRetries);
  auto first = parseResponse(firstBody, context);
  if (itemsAddUp(first) || config_.rescanEffort.empty()) {
    co_return first;
  }
  // Items that don't add up mean a misread, typically a receipt that prints prices off their
  // item's line; the prompt's own add-up check doesn't reliably catch it at low effort. The rescan
  // replaces the first scan only if its own items add up: a rescan that is also off can be worse
  // (seen: three dishes lumped into one line). If it fails or is off, the first scan stands, and
  // the review screen can see the mismatch since the stored items and total still disagree.
  auto rescanConfig = config_;
  rescanConfig.effort = config_.rescanEffort;
  const auto rescanRequest = buildRequest(images, context, rescanConfig).dump();
  std::string error;
  try {
    const auto secondBody = co_await send(rescanRequest, /*maxRetries=*/0);
    auto second = parseResponse(secondBody, context);
    if (itemsAddUp(second)) {
      second.raw["first_scan"] = std::move(first.raw);
      co_return second;
    }
    first.raw["rescan"] = std::move(second.raw);
    co_return first;
  } catch (const std::exception &e) {
    error = e.what();
  }
  LOG_WARN << "rescan after an item/total mismatch failed, keeping the first scan: " << error;
  first.raw["rescan_error"] = error;
  co_return first;
}

drogon::Task<std::string> AnthropicScanProvider::send(std::string body, int maxRetries) {
  for (int attempt = 0;; ++attempt) {
    auto req = drogon::HttpRequest::newHttpRequest();
    req->setMethod(drogon::Post);
    req->setPath("/v1/messages");
    req->addHeader("x-api-key", config_.apiKey);
    req->addHeader("anthropic-version", kApiVersion);
    req->setContentTypeCode(drogon::CT_APPLICATION_JSON);
    req->setBody(body);

    // A timeout or network failure throws drogon::HttpException: infrastructure, so it spends
    // the receipt's reclaim budget rather than a scan attempt (see ScanError).
    auto resp = co_await client_->sendRequestCoro(req, config_.timeoutSeconds);
    const auto status = resp->statusCode();
    if (status == drogon::k200OK) {
      co_return std::string(resp->body());
    }
    const auto message = "Anthropic API HTTP " + std::to_string(static_cast<int>(status)) + ": " +
                         apiErrorMessage(std::string(resp->body()));
    if (!retryable(status)) {
      throw ScanError(message);  // a request this receipt made invalid (e.g. an image the API rejects)
    }
    if (attempt >= maxRetries) {
      throw std::runtime_error(message);  // rate limit or outage: not evidence against this receipt
    }
    double delay = 2 << attempt; // 2s, then 4s
    if (const auto &retryAfter = resp->getHeader("retry-after"); !retryAfter.empty()) {
      try {
        delay = std::clamp(std::stod(retryAfter), 0.0, 30.0);
      } catch (const std::exception &) {
        // an HTTP-date form; keep the backoff
      }
    }
    LOG_WARN << message << " -- retrying in " << delay << "s";
    co_await drogon::sleepCoro(loop_, delay);
  }
}

}  // namespace receipt_scanner::scan
