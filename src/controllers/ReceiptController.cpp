#include "ReceiptController.h"

#include "receipts/AppServices.h"
#include "receipts/ReceiptService.h"

#include <drogon/MultiPart.h>
#include <drogon/drogon.h>
#include <nlohmann/json.hpp>

#include <algorithm>

namespace receipt_scanner {

namespace {

using receipt_scanner::receipts::ImageConflict;
using receipt_scanner::receipts::ReceiptNotFoundError;
using receipt_scanner::receipts::ReceiptService;
using receipt_scanner::receipts::ReceiptSummary;
using receipt_scanner::receipts::TooManyImagesError;

constexpr size_t kMaxImageBytes = 15 * 1024 * 1024; // 15MB/image, app-layer copy of Caddy's limit (spec)

drogon::HttpResponsePtr errorResponse(drogon::HttpStatusCode status, const std::string &code,
                                       const std::string &message) {
  nlohmann::json body{{"error", {{"code", code}, {"message", message}}}};
  auto resp = drogon::HttpResponse::newHttpResponse();
  resp->setStatusCode(status);
  resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
  resp->setBody(body.dump());
  return resp;
}

drogon::HttpResponsePtr jsonResponse(drogon::HttpStatusCode status, const nlohmann::json &body) {
  auto resp = drogon::HttpResponse::newHttpResponse();
  resp->setStatusCode(status);
  resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
  resp->setBody(body.dump());
  return resp;
}

drogon::HttpResponsePtr conflictResponse(const ImageConflict &conflict) {
  nlohmann::json body{{"error",
                        {{"code", "duplicate_image"},
                         {"message", "an image with identical content already exists"},
                         {"duplicate_of", {{"receipt_id", conflict.receiptId}, {"image_id", conflict.imageId}}}}}};
  auto resp = drogon::HttpResponse::newHttpResponse();
  resp->setStatusCode(drogon::k409Conflict);
  resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
  resp->setBody(body.dump());
  return resp;
}

nlohmann::json toJson(const ReceiptSummary &r) {
  nlohmann::json j{
      {"id", r.id},
      {"store_name_raw", r.storeNameRaw ? nlohmann::json(*r.storeNameRaw) : nlohmann::json(nullptr)},
      {"merchant", r.merchant ? nlohmann::json(*r.merchant) : nlohmann::json(nullptr)},
      {"purchase_date", r.purchaseDate ? nlohmann::json(*r.purchaseDate) : nlohmann::json(nullptr)},
      {"total_cents", r.totalCents},
      {"direction", r.direction},
      {"currency", r.currency},
      {"kind", r.kind},
      {"scan_state", r.scanState},
      {"tax_deductible", r.taxDeductible},
      {"note", r.note ? nlohmann::json(*r.note) : nlohmann::json(nullptr)},
      {"source", r.source},
  };
  return j;
}

int64_t currentUserId(const drogon::HttpRequestPtr &req) {
  return req->attributes()->get<int64_t>("user_id");
}

// Returns nullopt (and has already written an error response via callback) if any file is
// missing, oversized, or there are none at all -- the caller just returns in that case.
std::optional<std::vector<std::string>> extractImageFiles(
    const drogon::HttpRequestPtr &req, const std::function<void(const drogon::HttpResponsePtr &)> &callback) {
  drogon::MultiPartParser parser;
  if (parser.parse(req) != 0 || parser.getFiles().empty()) {
    callback(errorResponse(drogon::k400BadRequest, "invalid_request", "expected one or more multipart image files"));
    return std::nullopt;
  }

  std::vector<std::string> images;
  for (const auto &file : parser.getFiles()) {
    if (file.fileLength() > kMaxImageBytes) {
      callback(errorResponse(drogon::k400BadRequest, "file_too_large", "each image must be 15MB or smaller"));
      return std::nullopt;
    }
    images.emplace_back(file.fileData(), file.fileLength());
  }
  return images;
}

bool forceRequested(const drogon::HttpRequestPtr &req) {
  return req->getParameter("force") == "true";
}

} // namespace

drogon::Task<> ReceiptController::upload(drogon::HttpRequestPtr req,
                                          std::function<void(const drogon::HttpResponsePtr &)> callback) {
  auto images = extractImageFiles(req, callback);
  if (!images.has_value()) {
    co_return;
  }

  ReceiptService service(drogon::app().getDbClient(), receipts::imageStore(), receipts::imageNormalizer());
  try {
    auto result = co_await service.uploadReceipt(currentUserId(req), std::move(*images), forceRequested(req));
    if (std::holds_alternative<ImageConflict>(result)) {
      callback(conflictResponse(std::get<ImageConflict>(result)));
      co_return;
    }
    callback(jsonResponse(drogon::k201Created, {{"id", std::get<int64_t>(result)}}));
  } catch (const std::exception &e) {
    callback(errorResponse(drogon::k400BadRequest, "invalid_image", e.what()));
  }
}

drogon::Task<> ReceiptController::addImages(drogon::HttpRequestPtr req,
                                             std::function<void(const drogon::HttpResponsePtr &)> callback,
                                             int64_t id) {
  auto images = extractImageFiles(req, callback);
  if (!images.has_value()) {
    co_return;
  }

  ReceiptService service(drogon::app().getDbClient(), receipts::imageStore(), receipts::imageNormalizer());
  try {
    auto result = co_await service.addImages(currentUserId(req), id, std::move(*images), forceRequested(req));
    if (std::holds_alternative<ImageConflict>(result)) {
      callback(conflictResponse(std::get<ImageConflict>(result)));
      co_return;
    }
    callback(jsonResponse(drogon::k200OK, {{"status", "ok"}}));
  } catch (const ReceiptNotFoundError &) {
    callback(errorResponse(drogon::k404NotFound, "not_found", "receipt not found"));
  } catch (const TooManyImagesError &) {
    callback(errorResponse(drogon::k400BadRequest, "too_many_images", "adding these images would exceed the per-receipt cap"));
  } catch (const std::exception &e) {
    callback(errorResponse(drogon::k400BadRequest, "invalid_image", e.what()));
  }
}

drogon::Task<> ReceiptController::listImages(drogon::HttpRequestPtr req,
                                              std::function<void(const drogon::HttpResponsePtr &)> callback,
                                              int64_t id) {
  ReceiptService service(drogon::app().getDbClient(), receipts::imageStore(), receipts::imageNormalizer());
  try {
    auto images = co_await service.listImages(currentUserId(req), id);
    nlohmann::json arr = nlohmann::json::array();
    for (const auto &img : images) {
      arr.push_back({{"id", img.id}, {"created_at", img.createdAt}});
    }
    callback(jsonResponse(drogon::k200OK, arr));
  } catch (const ReceiptNotFoundError &) {
    callback(errorResponse(drogon::k404NotFound, "not_found", "receipt not found"));
  }
}

drogon::Task<> ReceiptController::list(drogon::HttpRequestPtr req,
                                        std::function<void(const drogon::HttpResponsePtr &)> callback) {
  int limit = 50;
  int offset = 0;
  if (!req->getParameter("limit").empty()) {
    limit = std::clamp(std::stoi(req->getParameter("limit")), 1, 200);
  }
  if (!req->getParameter("offset").empty()) {
    offset = std::max(0, std::stoi(req->getParameter("offset")));
  }

  ReceiptService service(drogon::app().getDbClient(), receipts::imageStore(), receipts::imageNormalizer());
  auto result = co_await service.listReceipts(currentUserId(req), limit, offset);
  nlohmann::json arr = nlohmann::json::array();
  for (const auto &r : result) {
    arr.push_back(toJson(r));
  }
  callback(jsonResponse(drogon::k200OK, arr));
}

drogon::Task<> ReceiptController::get(drogon::HttpRequestPtr req,
                                       std::function<void(const drogon::HttpResponsePtr &)> callback, int64_t id) {
  ReceiptService service(drogon::app().getDbClient(), receipts::imageStore(), receipts::imageNormalizer());
  auto result = co_await service.getReceipt(currentUserId(req), id);
  if (!result.has_value()) {
    callback(errorResponse(drogon::k404NotFound, "not_found", "receipt not found"));
    co_return;
  }
  callback(jsonResponse(drogon::k200OK, toJson(*result)));
}

drogon::Task<> ReceiptController::remove(drogon::HttpRequestPtr req,
                                          std::function<void(const drogon::HttpResponsePtr &)> callback, int64_t id) {
  ReceiptService service(drogon::app().getDbClient(), receipts::imageStore(), receipts::imageNormalizer());
  auto deleted = co_await service.deleteReceipt(currentUserId(req), id);
  if (!deleted) {
    callback(errorResponse(drogon::k404NotFound, "not_found", "receipt not found"));
    co_return;
  }
  callback(jsonResponse(drogon::k200OK, {{"status", "ok"}}));
}

drogon::Task<> ReceiptController::image(drogon::HttpRequestPtr req,
                                         std::function<void(const drogon::HttpResponsePtr &)> callback, int64_t id) {
  ReceiptService service(drogon::app().getDbClient(), receipts::imageStore(), receipts::imageNormalizer());
  auto location = co_await service.findImage(id);
  // Ownership check happens here, not in the service: findImage deliberately doesn't take a
  // userId so it can return the owner for comparison -- missing OR not-owned both become a
  // plain 404 (spec API conventions: no existence leak).
  if (!location.has_value() || location->ownerUserId != currentUserId(req)) {
    callback(errorResponse(drogon::k404NotFound, "not_found", "image not found"));
    co_return;
  }

  auto bytes = co_await receipts::imageStore().get(location->imageKey);
  auto resp = drogon::HttpResponse::newHttpResponse();
  resp->setStatusCode(drogon::k200OK);
  resp->setContentTypeString("image/webp"); // every stored image is WebP -- normalization always re-encodes to it
  resp->addHeader("Cache-Control", "private, max-age=31536000, immutable");
  resp->setBody(std::move(bytes));
  callback(resp);
}

} // namespace receipt_scanner
