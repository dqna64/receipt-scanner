// Golden-set harness (spec Scanner quality): runs each labelled case through the production image
// normalizer and the Anthropic adapter, then reports per-field accuracy against the spec targets.
// Re-run on every prompt, model or provider change. Every run calls the real API and costs money.
//
//   golden_harness sample-receipts/golden.json
//
// golden.json (image paths are relative to its own directory; every `expected` field is optional,
// and null means the document doesn't show it). A case is scored only once its labels have been
// checked by a person: one with no `expected`, or with "checked": false (a draft copied from a
// scan), is scanned and printed but not scored:
//   {"cases": [{"id": "woolies", "images": ["IMG_7487.jpeg"],
//               "expected": {"kind": "purchase", "total": 23.50, "currency": "AUD",
//                            "purchase_date": "2026-10-01", "merchant": "Woolworths",
//                            "items": [{"name": "Bananas", "amount": 4.35, "category": "groceries"}]}}]}
// An item may also carry "informational": true (payslip gross/tax/super lines) and "quantity"
// (null: the line prints none). Scanned items are paired with labelled ones by amount; each pair's
// category, and quantity where labelled, is scored, and the share of labelled items found is reported. Item categories must be on the default category lists (checked first).
//
// Writes results-<UTC time>.json next to golden.json: each case's scanned values in the same
// `expected` shape (so it doubles as a draft to correct into labels) plus the raw reply.

#include "scan/AnthropicScanProvider.h"
#include "seeds/DefaultSeeds.h"
#include "storage/ImageNormalizer.h"

#include <drogon/drogon.h>
#include <trantor/net/EventLoopThread.h>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>

namespace fs = std::filesystem;
using receipt_scanner::scan::AnthropicScanProvider;
using receipt_scanner::scan::ScanContext;
using receipt_scanner::scan::ScanResult;

namespace {

// Haiku 5.5 list prices, USD per million tokens (prompts up to 100K tokens).
constexpr double kInputPerMTok = 0.10;
constexpr double kOutputPerMTok = 0.50;

struct Tally {
  int correct = 0;
  int total = 0;
};

// Field -> target %, per document group. Kind and currency have no separate receipt target in
// the spec; doc-type's 98% (payslips) is applied to both groups.
const std::map<std::string, std::map<std::string, int>> kTargets = {
    {"receipts", {{"kind", 98}, {"total", 98}, {"purchase_date", 95}, {"merchant", 90}, {"item_category", 80}}},
    {"payslips", {{"kind", 98}, {"total", 98}, {"purchase_date", 95}, {"merchant", 90}}},
};

std::string readFile(const fs::path &path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    throw std::runtime_error("can't read " + path.string());
  }
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
}

std::string squash(const std::string &s) {
  std::string out;
  for (unsigned char c : s) {
    if (std::isalnum(c)) {
      out.push_back(static_cast<char>(std::tolower(c)));
    }
  }
  return out;
}

// Lenient on purpose: "Woolworths" and "Woolworths Metro" are the same merchant for this
// measure; the stricter trigram threshold is the worker's job, tuned separately (19e).
bool merchantMatches(const std::string &expected, const std::optional<std::string> &scanned) {
  if (!scanned) {
    return false;
  }
  const auto a = squash(expected);
  const auto b = squash(*scanned);
  return !a.empty() && !b.empty() && (a.find(b) != std::string::npos || b.find(a) != std::string::npos);
}

nlohmann::json orNull(const std::optional<std::string> &value) {
  return value ? nlohmann::json(*value) : nlohmann::json(nullptr);
}

nlohmann::json asExpected(const ScanResult &result) {
  nlohmann::json items = nlohmann::json::array();
  for (const auto &item : result.items) {
    nlohmann::json line = {{"name", orNull(item.name)},
                           {"amount", static_cast<double>(item.amountCents) / 100},
                           {"category", orNull(item.category)}};
    if (item.quantity) {
      line["quantity"] = *item.quantity;
    }
    if (item.informational) {
      line["informational"] = true;
    }
    items.push_back(std::move(line));
  }
  return {
      {"kind", result.kind},
      {"total", static_cast<double>(result.totalCents) / 100},
      {"currency", result.currency},
      {"purchase_date", orNull(result.purchaseDate)},
      {"merchant", orNull(result.merchant)},
      {"items", std::move(items)},
  };
}

// A typo in a label (e.g. "dining" for "dining/takeaway") would silently count as a model error.
std::vector<std::string> unknownCategories(const nlohmann::json &golden, const ScanContext &context) {
  std::vector<std::string> problems;
  for (const auto &testCase : golden.at("cases")) {
    const auto expected = testCase.value("expected", nlohmann::json::object());
    for (const auto &item : expected.value("items", nlohmann::json::array())) {
      const auto category = item.value("category", nlohmann::json(nullptr));
      if (category.is_null()) {
        continue;
      }
      const auto name = category.get<std::string>();
      const auto known = [&](const std::vector<std::string> &list) {
        return std::find(list.begin(), list.end(), name) != list.end();
      };
      if (!known(context.expenseCategories) && !known(context.incomeCategories)) {
        problems.push_back(testCase.value("id", "?") + ": unknown category \"" + name + "\"");
      }
    }
  }
  return problems;
}

std::string utcStamp() {
  const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  std::ostringstream out;
  out << std::put_time(std::gmtime(&now), "%Y%m%dT%H%M%SZ");
  return out.str();
}

} // namespace

int main(int argc, char **argv) {
  if (argc != 2) {
    std::cerr << "usage: golden_harness <golden.json>\n";
    return 2;
  }
  const fs::path goldenPath = argv[1];
  const auto golden = nlohmann::json::parse(readFile(goldenPath));
  const auto baseDir = goldenPath.parent_path();

  trantor::EventLoopThread loopThread;
  loopThread.run();
  receipt_scanner::storage::ImageNormalizer normalizer;
  const auto config = AnthropicScanProvider::Config::fromEnv();
  AnthropicScanProvider provider(config, loopThread.getLoop());

  ScanContext context;
  context.expenseCategories.assign(receipt_scanner::seeds::kExpenseCategories.begin(),
                                   receipt_scanner::seeds::kExpenseCategories.end());
  context.incomeCategories.assign(receipt_scanner::seeds::kIncomeCategories.begin(),
                                  receipt_scanner::seeds::kIncomeCategories.end());

  if (const auto problems = unknownCategories(golden, context); !problems.empty()) {
    for (const auto &problem : problems) {
      std::cerr << problem << "\n";
    }
    std::cerr << "fix these labels first (no API calls were made)\n";
    return 2;
  }

  std::map<std::string, std::map<std::string, Tally>> tallies; // group -> field -> tally
  std::map<std::string, Tally> currency;                       // group -> currency (no target)
  int64_t inputTokens = 0;
  int64_t outputTokens = 0;
  nlohmann::json results = {{"model", config.model}, {"effort", config.effort}, {"cases", nlohmann::json::array()}};

  for (const auto &testCase : golden.at("cases")) {
    const auto id = testCase.value("id", testCase.at("images").at(0).get<std::string>());
    const auto expected = testCase.value("expected", nlohmann::json::object());
    const bool labelled = !expected.empty() && testCase.value("checked", true);
    const std::string group = expected.value("kind", "") == "income" ? "payslips" : "receipts";
    nlohmann::json record = {{"id", id}, {"images", testCase.at("images")}};

    std::optional<ScanResult> result;
    std::string error;
    try {
      std::vector<std::string> images;
      for (const auto &name : testCase.at("images")) {
        auto normalized =
            drogon::sync_wait(normalizer.normalize(readFile(baseDir / name.get<std::string>())));
        images.push_back(std::move(normalized.bytes));
      }
      result = drogon::sync_wait(provider.scanReceipt(std::move(images), context));
      const auto addUsage = [&](const nlohmann::json &raw) {
        inputTokens += raw.at("usage").value("input_tokens", 0);
        outputTokens += raw.at("usage").value("output_tokens", 0);
      };
      addUsage(result->raw);
      if (const auto first = result->raw.find("first_scan"); first != result->raw.end()) {
        addUsage(*first);
      }
      record["expected"] = asExpected(*result);
      record["raw"] = result->raw;
    } catch (const std::exception &e) {
      error = e.what();
      record["error"] = error;
    }
    results["cases"].push_back(record);

    std::cout << id << ": ";
    if (result && result->raw.contains("first_scan")) {
      std::cout << "(rescanned: items didn't add up) ";
    }
    if (!result) {
      std::cout << "ERROR " << error << "\n";
    } else if (!labelled) {
      std::cout << "not scored -> " << record["expected"].dump() << "\n";
    }
    if (!labelled) {
      continue;
    }

    std::vector<std::string> misses;
    auto score = [&](const std::string &field, bool ok, const std::string &got) {
      auto &tally = tallies[group][field];
      ++tally.total;
      if (ok) {
        ++tally.correct;
      } else {
        misses.push_back(field + " expected " + expected.at(field).dump() + " got " + got);
      }
    };
    if (expected.contains("kind")) {
      score("kind", result && result->kind == expected["kind"].get<std::string>(), result ? result->kind : "-");
    }
    if (expected.contains("total")) {
      const auto cents = std::llround(expected["total"].get<double>() * 100);
      score("total", result && result->totalCents == cents,
            result ? std::to_string(result->totalCents / 100.0) : "-");
    }
    // A null label means the document doesn't show the field, so the right answer is null.
    if (expected.contains("purchase_date")) {
      const auto &want = expected["purchase_date"];
      score("purchase_date",
            result && (want.is_null() ? !result->purchaseDate : result->purchaseDate == want.get<std::string>()),
            result ? result->purchaseDate.value_or("null") : "-");
    }
    if (expected.contains("merchant")) {
      const auto &want = expected["merchant"];
      score("merchant", result && (want.is_null() ? !result->merchant : merchantMatches(want, result->merchant)),
            result ? result->merchant.value_or("null") : "-");
    }
    if (expected.contains("items")) {
      // Pair each labelled item with an unused scanned item of the same amount, preferring the
      // same name. Item names and counts are best-effort (spec), so only "found" is reported;
      // categories are scored on the pairs (spec: measured on the items actually created).
      std::vector<bool> used(result ? result->items.size() : 0, false);
      for (const auto &want : expected["items"]) {
        const auto cents = std::llround(want.at("amount").get<double>() * 100);
        const auto wantName = squash(want.value("name", nlohmann::json("")).is_string()
                                         ? want["name"].get<std::string>()
                                         : std::string());
        std::optional<size_t> match;
        for (size_t i = 0; result && i < result->items.size(); ++i) {
          if (used[i] || result->items[i].amountCents != cents) {
            continue;
          }
          if (!match || squash(result->items[i].name.value_or("")) == wantName) {
            match = i;
          }
        }
        auto &found = tallies[group]["items_found"];
        ++found.total;
        const auto label = want.value("name", nlohmann::json("item")).dump() + " " + want["amount"].dump();
        if (!match) {
          misses.push_back("item " + label + " not found");
          continue;
        }
        ++found.correct;
        used[*match] = true;
        if (want.contains("quantity")) {
          const auto &got = result->items[*match].quantity;
          const auto &wantQuantity = want["quantity"];
          const bool ok = wantQuantity.is_null() ? !got : got && *got == wantQuantity.get<double>();
          auto &tally = tallies[group]["item_quantity"];
          ++tally.total;
          if (ok) {
            ++tally.correct;
          } else {
            misses.push_back("item " + label + " quantity expected " + wantQuantity.dump() + " got " +
                             (got ? nlohmann::json(*got).dump() : "null"));
          }
        }
        if (want.value("informational", false)) {
          continue;
        }
        const auto &got = result->items[*match].category;
        const auto &wantCategory = want.value("category", nlohmann::json(nullptr));
        const bool ok = wantCategory.is_null() ? !got : got == wantCategory.get<std::string>();
        auto &tally = tallies[group]["item_category"];
        ++tally.total;
        if (ok) {
          ++tally.correct;
        } else {
          misses.push_back("item " + label + " category expected " + wantCategory.dump() + " got " +
                           got.value_or("null"));
        }
      }
      const auto extra = std::count(used.begin(), used.end(), false);
      if (extra > 0) {
        misses.push_back(std::to_string(extra) + " scanned item(s) with no labelled match");
      }
    }
    if (expected.contains("currency")) {
      auto &tally = currency[group];
      ++tally.total;
      if (result && result->currency == expected["currency"].get<std::string>()) {
        ++tally.correct;
      } else {
        misses.push_back("currency expected " + expected["currency"].dump() + " got " +
                         (result ? result->currency : "-"));
      }
    }
    std::cout << (misses.empty() ? "ok" : "") << "\n";
    for (const auto &miss : misses) {
      std::cout << "    " << miss << "\n";
    }
  }

  std::cout << "\n" << std::left << std::setw(10) << "group" << std::setw(15) << "field" << std::setw(12) << "accuracy"
            << "target\n";
  for (const auto &[group, fields] : tallies) {
    for (const auto &[field, tally] : fields) {
      const double pct = 100.0 * tally.correct / tally.total;
      const auto &targets = kTargets.at(group);
      std::cout << std::setw(10) << group << std::setw(15) << field << std::setw(12)
                << (std::to_string(tally.correct) + "/" + std::to_string(tally.total)) << std::fixed
                << std::setprecision(0) << pct << "%";
      if (auto target = targets.find(field); target != targets.end()) {
        std::cout << " (target " << target->second << "%)" << (pct >= target->second ? "" : "  BELOW");
      } else {
        std::cout << " (no target)";
      }
      std::cout << "\n";
    }
  }
  for (const auto &[group, tally] : currency) {
    std::cout << std::setw(10) << group << std::setw(15) << "currency" << tally.correct << "/" << tally.total
              << " (no target)\n";
  }
  const double cost = inputTokens / 1e6 * kInputPerMTok + outputTokens / 1e6 * kOutputPerMTok;
  std::cout << "\ntokens: " << inputTokens << " in, " << outputTokens << " out; ~$" << std::setprecision(4) << cost
            << " at Haiku 5.5 list price\n";

  const auto resultsPath = baseDir / ("results-" + utcStamp() + ".json");
  std::ofstream(resultsPath) << results.dump(2) << "\n";
  std::cout << "wrote " << resultsPath.string() << "\n";
  return 0;
}
