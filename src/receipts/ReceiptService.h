#pragma once

#include "storage/ImageNormalizer.h"
#include "storage/ImageStore.h"

#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace receipt_scanner::receipts {

// Identifies the existing image a dedup check hit (spec Dedupe: the exact-duplicate upload
// check, scoped (user_id, image_sha256)). Returned instead of creating anything when a hash
// collision is found and the caller didn't pass force=true.
struct ImageConflict {
  int64_t receiptId;
  int64_t imageId;
};

struct ReceiptSummary {
  int64_t id;
  std::optional<std::string> storeNameRaw;
  std::optional<std::string> merchant;
  std::optional<std::string> purchaseDate;
  int64_t totalCents;
  std::string direction;
  std::string currency;
  std::string kind;
  std::string scanState;
  bool taxDeductible;
  std::optional<std::string> note;
  std::string source;
};

struct ImageRef {
  int64_t id;
  std::string createdAt;
};

struct ImageLocation {
  int64_t ownerUserId;
  std::string imageKey;
};

class ReceiptNotFoundError : public std::runtime_error {
public:
  ReceiptNotFoundError() : std::runtime_error("receipt not found") {}
};

class TooManyImagesError : public std::runtime_error {
public:
  TooManyImagesError() : std::runtime_error("image cap exceeded") {}
};

// Business logic, deliberately independent of HTTP/Drogon controllers (same reasoning as
// AuthService -- directly unit-testable against real Postgres/MinIO, no mocks).
class ReceiptService {
public:
  ReceiptService(drogon::orm::DbClientPtr db, storage::ImageStore &imageStore,
                 const storage::ImageNormalizer &normalizer)
      : db_(std::move(db)), imageStore_(imageStore), normalizer_(normalizer) {}

  // POST /receipts: creates a new receipt with one or more images. Every image is
  // normalized + hashed + dedup-checked BEFORE anything is written (not even to S3) if
  // force is false -- a rejected upload truly writes nothing. On success, returns the new
  // receipt's id; kind/direction are the provisional 'purchase'/'outflow' defaults (spec:
  // "receipts.kind: purchase (default)") until Step 10's scanner classifies the document.
  drogon::Task<std::variant<int64_t, ImageConflict>> uploadReceipt(int64_t userId,
                                                                    std::vector<std::string> images, bool force);

  // POST /receipts/:id/images: adds images to an existing receipt (imageless or not).
  // Throws ReceiptNotFoundError if receiptId isn't owned by userId. Throws
  // TooManyImagesError if adding would exceed the configured per-receipt cap (spec decided
  // 1a -- an add-time-only cap on LLM vision cost, not a storage limit; merge is exempt,
  // but merge doesn't exist yet -- Step 14).
  drogon::Task<std::variant<std::monostate, ImageConflict>> addImages(int64_t userId, int64_t receiptId,
                                                                       std::vector<std::string> images, bool force);

  drogon::Task<std::optional<ReceiptSummary>> getReceipt(int64_t userId, int64_t receiptId);
  drogon::Task<std::vector<ReceiptSummary>> listReceipts(int64_t userId, int limit, int offset);

  // Throws ReceiptNotFoundError if receiptId isn't owned by userId (callers translate to a
  // 404, never a 403 -- spec API conventions, no existence leak).
  drogon::Task<std::vector<ImageRef>> listImages(int64_t userId, int64_t receiptId);

  // false if receiptId doesn't exist or isn't owned by userId. On success, the DB row (and
  // its receipt_images rows, via ON DELETE CASCADE) is already gone before this returns;
  // the S3 objects are then removed best-effort -- a lingering object after a failed S3
  // delete is wasted storage, not a correctness problem, since the DB (source of truth for
  // what the app considers to exist) is already clean.
  drogon::Task<bool> deleteReceipt(int64_t userId, int64_t receiptId);

  // For the image proxy (GET /images/:id): looks up the image regardless of who's asking,
  // so the controller can compare ownerUserId itself and decide 404 vs stream the bytes.
  drogon::Task<std::optional<ImageLocation>> findImage(int64_t imageId);

private:
  drogon::orm::DbClientPtr db_;
  storage::ImageStore &imageStore_;
  const storage::ImageNormalizer &normalizer_;

  struct PreparedImage {
    storage::NormalizedImage normalized;
    std::string sha256Hex;
  };

  // Normalizes + hashes + dedup-checks a batch against userId's existing images (scoped
  // (user_id, image_sha256), matching the spec's dup-check scope). Returns the first
  // conflict found, or everything ready to store -- nothing is written to S3 or Postgres
  // by this step either way.
  drogon::Task<std::variant<std::vector<PreparedImage>, ImageConflict>> prepareImages(
      int64_t userId, const std::vector<std::string> &rawImages, bool force);
};

} // namespace receipt_scanner::receipts
