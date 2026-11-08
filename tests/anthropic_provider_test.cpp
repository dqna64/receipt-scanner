#include "receipts/Kinds.h"
#include "scan/AnthropicScanProvider.h"

#include <catch2/catch_test_macros.hpp>
#include <drogon/HttpClient.h>
#include <trantor/net/EventLoopThread.h>

#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

using receipt_scanner::scan::AnthropicScanProvider;
using receipt_scanner::scan::ScanContext;
using receipt_scanner::scan::ScanError;
using receipt_scanner::scan::ScanResult;

namespace {

const std::string kWebp = std::string("RIFF\x10\x00\x00\x00WEBPVP8 ", 16);
const std::string kJpeg = std::string("\xFF\xD8\xFF\xE0 jpeg", 9);

ScanContext extractionContext() {
  ScanContext context;
  context.expenseCategories = {"groceries", "fuel", "other"};
  context.incomeCategories = {"salary", "Other"};
  context.tags = {"japan-trip"};
  return context;
}

std::vector<std::string> images(std::initializer_list<std::string> list) {
  return std::vector<std::string>(list);
}

// A Messages API response whose text block is `output`, preceded by a thinking block (adaptive
// thinking is on by default for Haiku 5.5, and the parser must skip it).
std::string response(const nlohmann::json &output, const std::string &stopReason = "end_turn") {
  nlohmann::json body = {
      {"id", "msg_test"},
      {"type", "message"},
      {"model", "claude-haiku-5-5"},
      {"stop_reason", stopReason},
      {"usage", {{"input_tokens", 1800}, {"output_tokens", 240}}},
      {"content", nlohmann::json::array({{{"type", "thinking"}, {"thinking", ""}, {"signature", "sig"}},
                                         {{"type", "text"}, {"text", output.dump()}}})},
  };
  return body.dump();
}

nlohmann::json purchaseOutput() {
  return {
      {"unreadable_reason", nullptr},
      {"kind", "purchase"},
      {"currency", "aud"},
      {"total", 23.5},
      {"merchant", "Woolworths"},
      {"store_name_raw", "WOOLWORTHS METRO 1234"},
      {"purchase_date", "2026-10-01"},
      {"items",
       nlohmann::json::array({
           {{"name", "Bananas"}, {"quantity", 2}, {"unit_price", 2.18}, {"amount", 4.35},
            {"category", "groceries"}, {"informational", false}, {"tags", nlohmann::json::array()}},
           {{"name", "Member saving"}, {"quantity", nullptr}, {"unit_price", nullptr}, {"amount", -1.0},
            {"category", nullptr}, {"informational", false}, {"tags", nlohmann::json::array()}},
           {{"name", "  "}, {"quantity", 0}, {"unit_price", nullptr}, {"amount", 20.15},
            {"category", nullptr}, {"informational", false}, {"tags", nlohmann::json::array({"japan-trip"})}},
       })},
  };
}

std::vector<std::string> enumOf(const nlohmann::json &nullableSchema) {
  return nullableSchema.at("anyOf").at(0).at("enum").get<std::vector<std::string>>();
}

} // namespace

TEST_CASE("anthropic: the request sends every image in order, then the instructions", "[anthropic]") {
  AnthropicScanProvider::Config config;
  auto request = AnthropicScanProvider::buildRequest(images({kWebp, kJpeg}), extractionContext(), config);

  CHECK(request["model"] == "claude-haiku-5-5");
  CHECK(request["output_config"]["effort"] == "low");
  CHECK(request["output_config"]["format"]["type"] == "json_schema");
  const auto &content = request["messages"][0]["content"];
  REQUIRE(content.size() == 5);
  CHECK(content[0]["text"] == "Image 1:");
  CHECK(content[1]["source"]["media_type"] == "image/webp");
  CHECK(content[1]["source"]["data"] == drogon::utils::base64Encode(
                                           reinterpret_cast<const unsigned char *>(kWebp.data()), kWebp.size()));
  CHECK(content[2]["text"] == "Image 2:");
  CHECK(content[3]["source"]["media_type"] == "image/jpeg");
  const auto instructions = content[4]["text"].get<std::string>();
  CHECK(instructions.find("2 images") != std::string::npos);
  CHECK(instructions.find("Classify its kind") != std::string::npos);
  CHECK(instructions.find("Expense categories: groceries, fuel, other") != std::string::npos);
  CHECK(instructions.find("Income categories: salary, Other") != std::string::npos);
  CHECK(instructions.find("user_notes") == std::string::npos); // no notes, no section
}

TEST_CASE("anthropic: the user's scanner notes follow the instructions, if there are any", "[anthropic]") {
  AnthropicScanProvider::Config config;
  auto context = extractionContext();
  context.scannerNotes = "Yee3 is junk food.";
  auto request = AnthropicScanProvider::buildRequest(images({kWebp}), context, config);
  const auto instructions = request["messages"][0]["content"].back()["text"].get<std::string>();
  CHECK(instructions.find("<user_notes>\nYee3 is junk food.\n</user_notes>") != std::string::npos);

  context.scannerNotes = " \n ";
  request = AnthropicScanProvider::buildRequest(images({kWebp}), context, config);
  CHECK(request["messages"][0]["content"].back()["text"].get<std::string>().find("user_notes") == std::string::npos);
}

TEST_CASE("anthropic: extraction asks for the kind and offers both category sides", "[anthropic]") {
  AnthropicScanProvider::Config config;
  auto request = AnthropicScanProvider::buildRequest(images({kWebp}), extractionContext(), config);
  const auto &schema = request["output_config"]["format"]["schema"];

  CHECK(schema["properties"]["kind"]["enum"] == nlohmann::json({"purchase", "refund", "reimbursement", "income"}));
  const auto &required = schema["required"];
  CHECK(std::find(required.begin(), required.end(), "kind") != required.end());
  CHECK(schema["additionalProperties"] == false);
  const auto &item = schema["properties"]["items"]["items"];
  // "Other" duplicates "other" case-insensitively, so it appears once.
  CHECK(enumOf(item["properties"]["category"]) == std::vector<std::string>{"groceries", "fuel", "other", "salary"});
  CHECK(item["properties"]["tags"]["items"]["enum"] == nlohmann::json({"japan-trip"}));
  CHECK(request["messages"][0]["content"].size() == 2); // one image needs no "Image 1:" label
}

TEST_CASE("anthropic: the prompt and kind enum come from the scannable kinds in Kinds.h", "[anthropic]") {
  AnthropicScanProvider::Config config;
  auto request = AnthropicScanProvider::buildRequest(images({kWebp}), extractionContext(), config);
  const auto system = request["system"].get<std::string>();
  for (const auto &kind : request["output_config"]["format"]["schema"]["properties"]["kind"]["enum"]) {
    const auto name = kind.get<std::string>();
    const auto *rules = receipt_scanner::receipts::findKind(name);
    REQUIRE(rules);
    CHECK(rules->scannable());
    CHECK(system.find("- " + name + ": ") != std::string::npos);
    CHECK(system.find("For " + name + ", ") != std::string::npos);
  }
  CHECK(system.find("expense categories for purchase, refund, reimbursement; income categories for income") != std::string::npos);
  CHECK(system.find("For income:\n- merchant and store_name_raw are the employer") != std::string::npos);
}

TEST_CASE("anthropic: verify mode states the known kind and offers only that kind's categories", "[anthropic]") {
  AnthropicScanProvider::Config config;
  auto context = extractionContext();
  context.knownKind = "income";
  context.tags.clear();
  auto request = AnthropicScanProvider::buildRequest(images({kWebp}), context, config);
  const auto &schema = request["output_config"]["format"]["schema"];

  CHECK_FALSE(schema["properties"].contains("kind"));
  const auto &item = schema["properties"]["items"]["items"];
  CHECK(enumOf(item["properties"]["category"]) == std::vector<std::string>{"salary", "Other"});
  CHECK_FALSE(item["properties"].contains("tags")); // no tags -> no empty enum
  const auto instructions = request["messages"][0]["content"][1]["text"].get<std::string>();
  CHECK(instructions.find("known to be of kind \"income\"") != std::string::npos);
  CHECK(instructions.find("Expense categories") == std::string::npos);
}

TEST_CASE("anthropic: an empty category list becomes a null-only schema, not an empty enum", "[anthropic]") {
  AnthropicScanProvider::Config config;
  ScanContext context;
  context.knownKind = "purchase";
  auto request = AnthropicScanProvider::buildRequest(images({kWebp}), context, config);
  const auto &category = request["output_config"]["format"]["schema"]["properties"]["items"]["items"]["properties"]["category"];
  CHECK(category == nlohmann::json({{"type", "null"}}));
}

TEST_CASE("anthropic: an image in an unsupported format is a ScanError", "[anthropic]") {
  AnthropicScanProvider::Config config;
  CHECK_THROWS_AS(AnthropicScanProvider::buildRequest(images({"not an image"}), extractionContext(), config),
                  ScanError);
}

TEST_CASE("anthropic: a reply is parsed into cents, skipping the thinking block", "[anthropic]") {
  auto result = AnthropicScanProvider::parseResponse(response(purchaseOutput()), extractionContext());

  CHECK(result.kind == "purchase");
  CHECK(result.currency == "aud"); // uppercased by the worker's validation, not here
  CHECK(result.totalCents == 2350);
  CHECK(result.merchant == "Woolworths");
  CHECK(result.storeNameRaw == "WOOLWORTHS METRO 1234");
  CHECK(result.purchaseDate == "2026-10-01");
  // The negative "Member saving" line stays negative; a blank name and a zero quantity become empty.
  REQUIRE(result.items.size() == 3);
  CHECK(result.items[0].amountCents == 435); // 4.35 * 100 is 434.999... in binary floating point
  CHECK(result.items[0].unitPriceCents == 218);
  CHECK(result.items[0].quantity == 2.0);
  CHECK(result.items[0].category == "groceries");
  CHECK(result.items[1].amountCents == -100);
  CHECK_FALSE(result.items[2].name);
  CHECK_FALSE(result.items[2].quantity);
  CHECK(result.items[2].tags == std::vector<std::string>{"japan-trip"});
  CHECK(result.raw["provider"] == "anthropic");
  CHECK(result.raw["usage"]["input_tokens"] == 1800);
  CHECK(result.raw["output"]["total"] == 23.5);

  auto withZeroLine = purchaseOutput();
  withZeroLine["items"].push_back({{"name", "Docket Deal"}, {"quantity", nullptr}, {"unit_price", nullptr},
                                   {"amount", -0.0}, {"category", nullptr}, {"informational", false}});
  CHECK(AnthropicScanProvider::parseResponse(response(withZeroLine), extractionContext()).items.size() == 3);
}

TEST_CASE("anthropic: a refund docket's negative figures are kept as magnitudes", "[anthropic]") {
  auto output = purchaseOutput();
  output["kind"] = "refund";
  output["total"] = -23.5;
  auto result = AnthropicScanProvider::parseResponse(response(output), extractionContext());

  CHECK(result.totalCents == 2350);
  REQUIRE(result.items.size() == 3);
  CHECK(result.items[1].amountCents == 100);
}

TEST_CASE("anthropic: verify mode takes the kind from the context", "[anthropic]") {
  auto context = extractionContext();
  context.knownKind = "transfer";
  auto output = purchaseOutput();
  output.erase("kind");
  auto result = AnthropicScanProvider::parseResponse(response(output), context);
  CHECK(result.kind == "transfer");
}

TEST_CASE("anthropic: unusable replies are ScanErrors", "[anthropic]") {
  const auto context = extractionContext();

  SECTION("a refusal") {
    CHECK_THROWS_AS(AnthropicScanProvider::parseResponse(response(purchaseOutput(), "refusal"), context), ScanError);
  }
  SECTION("a reply cut off at max_tokens") {
    CHECK_THROWS_AS(AnthropicScanProvider::parseResponse(response(purchaseOutput(), "max_tokens"), context),
                    ScanError);
  }
  SECTION("a document the model reports as unreadable") {
    auto output = purchaseOutput();
    output["unreadable_reason"] = "photo is too blurry";
    CHECK_THROWS_AS(AnthropicScanProvider::parseResponse(response(output), context), ScanError);
  }
  SECTION("a reply missing a required field") {
    auto output = purchaseOutput();
    output.erase("total");
    CHECK_THROWS_AS(AnthropicScanProvider::parseResponse(response(output), context), ScanError);
  }
  SECTION("a body that isn't JSON") {
    CHECK_THROWS_AS(AnthropicScanProvider::parseResponse("<html>bad gateway</html>", context), ScanError);
  }
  SECTION("a response with no text block") {
    nlohmann::json body = {{"stop_reason", "end_turn"}, {"content", nlohmann::json::array()}};
    CHECK_THROWS_AS(AnthropicScanProvider::parseResponse(body.dump(), context), ScanError);
  }
}

TEST_CASE("anthropic: items add up when the non-informational ones sum to the total", "[anthropic]") {
  auto result = AnthropicScanProvider::parseResponse(response(purchaseOutput()), extractionContext());
  CHECK(AnthropicScanProvider::itemsAddUp(result)); // 4.35 - 1.00 + 20.15, the discount line included

  result.items.push_back({.name = "Gross pay", .amountCents = 100000, .informational = true});
  CHECK(AnthropicScanProvider::itemsAddUp(result));

  result.totalCents += 1;
  CHECK_FALSE(AnthropicScanProvider::itemsAddUp(result));

  auto undiscounted = purchaseOutput(); // a discount left out of the items doesn't add up
  undiscounted["items"].erase(1);
  CHECK_FALSE(AnthropicScanProvider::itemsAddUp(
      AnthropicScanProvider::parseResponse(response(undiscounted), extractionContext())));

  auto surcharged = purchaseOutput(); // nor does a surcharge left out
  surcharged["total"] = 23.78;
  CHECK_FALSE(AnthropicScanProvider::itemsAddUp(
      AnthropicScanProvider::parseResponse(response(surcharged), extractionContext())));

  result.items.clear();
  CHECK(AnthropicScanProvider::itemsAddUp(result)); // nothing to check
}

namespace {

// Replaces the network: answers each request with the next reply (throwing once they run out, as
// an outage would) and records what was sent.
class StubbedProvider : public AnthropicScanProvider {
public:
  StubbedProvider(std::vector<std::string> replies, std::string rescanEffort = "medium")
      : AnthropicScanProvider(config(std::move(rescanEffort)), loop()), replies_(std::move(replies)) {}

  std::vector<nlohmann::json> requests;
  std::vector<int> maxRetries;

protected:
  drogon::Task<std::string> send(std::string body, int retries) override {
    requests.push_back(nlohmann::json::parse(body));
    maxRetries.push_back(retries);
    if (requests.size() > replies_.size()) {
      throw std::runtime_error("Anthropic API HTTP 529: overloaded");
    }
    co_return replies_[requests.size() - 1];
  }

private:
  static Config config(std::string rescanEffort) {
    Config config;
    config.apiKey = "test";
    config.rescanEffort = std::move(rescanEffort);
    return config;
  }
  static trantor::EventLoop *loop() {
    static trantor::EventLoopThread thread;
    static const bool started = (thread.run(), true);
    (void)started;
    return thread.getLoop();
  }

  std::vector<std::string> replies_;
};

nlohmann::json mismatchedOutput() {
  auto output = purchaseOutput();
  output["total"] = 30.0;
  return output;
}

}

TEST_CASE("anthropic: a scan whose items don't add up is rescanned once at the rescan effort", "[anthropic]") {
  SECTION("items that add up: one request, with the normal retries") {
    StubbedProvider provider({response(purchaseOutput())});
    auto result = drogon::sync_wait(provider.scanReceipt(images({kWebp}), extractionContext()));
    CHECK(provider.requests.size() == 1);
    CHECK(provider.maxRetries == std::vector<int>{2});
    CHECK(provider.requests[0]["output_config"]["effort"] == "low");
    CHECK_FALSE(result.raw.contains("first_scan"));
  }
  SECTION("a mismatch whose rescan is also off: the first scan stands, with the rescan's raw reply") {
    StubbedProvider provider({response(mismatchedOutput()), response(mismatchedOutput())});
    auto result = drogon::sync_wait(provider.scanReceipt(images({kWebp}), extractionContext()));
    CHECK(provider.requests.size() == 2);
    CHECK(result.raw.contains("rescan"));
    CHECK_FALSE(result.raw.contains("first_scan"));
  }
  SECTION("a mismatch: a rescan that adds up wins and keeps the first scan's raw reply") {
    auto rescanned = purchaseOutput();
    rescanned["merchant"] = "Woolworths Metro";
    StubbedProvider provider({response(mismatchedOutput()), response(rescanned)});
    auto result = drogon::sync_wait(provider.scanReceipt(images({kWebp}), extractionContext()));
    REQUIRE(provider.requests.size() == 2);
    CHECK(provider.requests[1]["output_config"]["effort"] == "medium");
    CHECK(provider.maxRetries == std::vector<int>{2, 0});
    CHECK(result.merchant == "Woolworths Metro");
    CHECK(result.raw["first_scan"]["usage"]["input_tokens"] == 1800);
  }
  SECTION("a mismatch whose rescan fails: the first scan stands") {
    StubbedProvider provider({response(mismatchedOutput())});
    auto result = drogon::sync_wait(provider.scanReceipt(images({kWebp}), extractionContext()));
    CHECK(provider.requests.size() == 2);
    CHECK(result.totalCents == 3000);
    CHECK(result.raw["rescan_error"] == "Anthropic API HTTP 529: overloaded");
  }
  SECTION("an empty rescan effort turns the rescan off") {
    StubbedProvider provider({response(mismatchedOutput())}, "");
    auto result = drogon::sync_wait(provider.scanReceipt(images({kWebp}), extractionContext()));
    CHECK(provider.requests.size() == 1);
    CHECK(result.totalCents == 3000);
  }
}

// Hidden ([.live]): needs the internet. Sends no API key, so it costs nothing; it proves the
// TLS handshake (certificate verification) works with this build's OpenSSL.
TEST_CASE("anthropic: the API is reachable over verified TLS", "[.live]") {
  trantor::EventLoopThread loopThread;
  loopThread.run();
  auto client = drogon::HttpClient::newHttpClient("https://api.anthropic.com", loopThread.getLoop());
  auto req = drogon::HttpRequest::newHttpRequest();
  req->setPath("/v1/models");
  auto [result, resp] = client->sendRequest(req, 20);
  REQUIRE(result == drogon::ReqResult::Ok);
  CHECK(resp->statusCode() == drogon::k401Unauthorized);
}

// Hidden ([.prompt]): prints the generated system prompt, for reviewing prompt changes.
TEST_CASE("anthropic: print the system prompt", "[.prompt]") {
  AnthropicScanProvider::Config config;
  auto request = AnthropicScanProvider::buildRequest(images({kWebp}), ScanContext{}, config);
  std::cout << request["system"].get<std::string>() << "\n";
}
