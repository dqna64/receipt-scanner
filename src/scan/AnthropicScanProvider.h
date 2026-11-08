#pragma once

#include "scan/ScanProvider.h"

#include <drogon/HttpClient.h>
#include <trantor/net/EventLoop.h>

#include <string>
#include <vector>

namespace receipt_scanner::scan {

// The Claude Messages API adapter, over raw HTTP (there is no C++ SDK). One multimodal call per
// receipt: every image, then the instructions; structured outputs constrain the reply to the
// extraction schema, whose category and tag enums are the requesting user's own lists.
class AnthropicScanProvider : public ScanProvider {
public:
  struct Config {
    std::string apiKey;
    std::string model = "claude-haiku-5-5";
    std::string baseUrl = "https://api.anthropic.com";
    // Per HTTP attempt. Must stay well under SCAN_RECLAIM_SECONDS, or a slow-but-alive call
    // gets re-queued and scanned twice (checked at startup, see ScanServices.cpp).
    double timeoutSeconds = 120;
    // Retries for 429 / 5xx / 529 only, with backoff; a timeout is not retried.
    int maxRetries = 2;
    int maxTokens = 16000;
    std::string effort = "low";
    // A scan whose items don't add up to its total is run once more at this effort, with no
    // retries (empty: never); the rescan is kept only if its items add up.
    std::string rescanEffort = "medium";

    // ANTHROPIC_API_KEY (required: throws std::runtime_error if unset or empty), ANTHROPIC_MODEL,
    // ANTHROPIC_BASE_URL, SCAN_TIMEOUT_SECONDS, ANTHROPIC_MAX_TOKENS, ANTHROPIC_EFFORT,
    // ANTHROPIC_RESCAN_EFFORT.
    static Config fromEnv();
  };

  AnthropicScanProvider(Config config, trantor::EventLoop *loop);

  drogon::Task<ScanResult> scanReceipt(std::vector<std::string> images, ScanContext context) override;

  // The two pure halves of a call, public so tests can check them without the network.
  static nlohmann::json buildRequest(const std::vector<std::string> &images, const ScanContext &context,
                                     const Config &config);
  // A 200 response body -> ScanResult. Throws ScanError for a refusal, a truncated reply, a reply
  // that isn't the schema's JSON, or a document the model reports as unreadable.
  static ScanResult parseResponse(const std::string &body, const ScanContext &context);
  // True when the non-informational items, discounts included, add up exactly to the total, or
  // there are no items.
  static bool itemsAddUp(const ScanResult &result);

protected:
  // POSTs one request body, retrying 408/429/5xx up to maxRetries times; returns the 200 body.
  // Virtual so tests can stand in for the network.
  virtual drogon::Task<std::string> send(std::string body, int maxRetries);

private:
  Config config_;
  trantor::EventLoop *loop_;
  drogon::HttpClientPtr client_;
};

} // namespace receipt_scanner::scan
