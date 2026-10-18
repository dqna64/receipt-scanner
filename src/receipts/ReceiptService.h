#pragma once

#include "receipts/Errors.h"
#include "receipts/LedgerInput.h"
#include "storage/ImageNormalizer.h"
#include "storage/ImageStore.h"

#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <memory>
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
  std::optional<int64_t> paymentMethodId;
  std::optional<int64_t> offsetsReceiptId;
  std::optional<double> fxRateToAud;
  std::optional<int64_t> audEquivalentCents;
  // ISO-8601 UTC. needsReview/isConfirmed come from v_receipt_state, the single source for the
  // timestamp predicates (spec Invariants) -- never recomputed here.
  std::optional<std::string> proposedAt;
  std::optional<std::string> reviewedAt;
  bool needsReview;
  bool isConfirmed;
  std::string createdAt;
  std::string updatedAt;
};

struct ImageRef {
  int64_t id;
  std::string createdAt;
};

struct ImageLocation {
  int64_t ownerUserId;
  std::string imageKey;
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
  // All-or-nothing on the DB side: any failure mid-batch rolls back the receipt and every
  // image row. Throws TooManyImagesError past the per-receipt cap (see addImages),
  // storage::ImageDecodeError for undecodable bytes.
  drogon::Task<std::variant<int64_t, ImageConflict>> uploadReceipt(int64_t userId,
                                                                    std::vector<std::string> images, bool force);

  // POST /receipts/:id/images: adds images to an existing receipt (imageless or not).
  // Throws ReceiptNotFoundError if receiptId isn't owned by userId. Throws
  // TooManyImagesError if adding would exceed the configured per-receipt cap (spec decided
  // 1a -- an add-time-only cap on LLM vision cost, not a storage limit; merge is exempt,
  // but merge doesn't exist yet -- Step 14). Same all-or-nothing rollback as uploadReceipt.
  drogon::Task<std::variant<std::monostate, ImageConflict>> addImages(int64_t userId, int64_t receiptId,
                                                                       std::vector<std::string> images, bool force);

  // POST /receipts/manual: a cash/no-receipt transaction, any of the five kinds. Born reviewed
  // (reviewed_at = now, proposed_at NULL, source = manual) so it counts immediately. Writes one
  // implicit item (name = merchant, amount = total, category = category_id) unless explicit
  // `items` are sent. purchase_date defaults to today in Australia/Sydney. Throws
  // ValidationError, incl. for a payment method / category / offset target / tag that isn't
  // userId's.
  drogon::Task<int64_t> createManual(int64_t userId, const nlohmann::json &body);

  // PATCH /receipts/:id: partial edit (see applyReceiptPatch). Editing never touches
  // reviewed_at, so a reviewed receipt stays reviewed (spec Review); "reviewed": true stamps it.
  // A kind change that would leave a categorized item on the wrong expense/income side is
  // rejected. Throws ReceiptNotFoundError, ValidationError.
  drogon::Task<ReceiptSummary> updateReceipt(int64_t userId, int64_t receiptId, const nlohmann::json &body);

  // nullopt if missing or not userId's.
  drogon::Task<std::optional<ReceiptSummary>> getReceipt(int64_t userId, int64_t receiptId);
  // Newest first, offset-paginated.
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

  // Normalizes + hashes a batch. Pure CPU work, no DB or S3 access.
  drogon::Task<std::vector<PreparedImage>> prepareImages(const std::vector<std::string> &rawImages);

  // The write path shared by uploadReceipt and addImages, all inside the caller's transaction:
  // lockUserImages first, then (unless force) findConflict, then storeImages.
  drogon::Task<> lockUserImages(const std::shared_ptr<drogon::orm::Transaction> &transaction, int64_t userId);
  // First image in the batch whose hash userId already has (scoped (user_id, image_sha256),
  // matching the spec's dup-check scope).
  drogon::Task<std::optional<ImageConflict>> findConflict(const std::shared_ptr<drogon::orm::Transaction> &transaction,
                                                          int64_t userId, const std::vector<PreparedImage> &prepared);
  drogon::Task<> storeImages(const std::shared_ptr<drogon::orm::Transaction> &transaction, int64_t userId, int64_t receiptId,
                             const std::vector<PreparedImage> &prepared);

  // The ownership checks shared by createManual and updateReceipt: payment method and offset
  // target must be userId's, and the offset target a purchase other than the receipt itself
  // (receiptId is nullopt on create).
  drogon::Task<> validateReferences(const std::shared_ptr<drogon::orm::Transaction> &transaction, int64_t userId,
                                    std::optional<int64_t> receiptId, const EditableReceipt &receipt);
};

} // namespace receipt_scanner::receipts
