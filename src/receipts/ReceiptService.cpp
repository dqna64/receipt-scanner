#include "ReceiptService.h"

#include "auth/TokenUtils.h"
#include "config/Env.h"

#include <drogon/drogon.h>
#include <sodium.h>

namespace receipt_scanner::receipts {

namespace {

// Same local-SHA256-hex pattern as auth/TokenUtils.cpp and storage/Sigv4Signer.cpp -- a
// handful of crypto helpers, each small enough that sharing one utility across modules
// would be more machinery than the three near-identical implementations it replaces.
std::string sha256Hex(const std::string &data) {
  unsigned char digest[crypto_hash_sha256_BYTES];
  crypto_hash_sha256(digest, reinterpret_cast<const unsigned char *>(data.data()), data.size());
  static const char *hex = "0123456789abcdef";
  std::string out;
  out.reserve(crypto_hash_sha256_BYTES * 2);
  for (unsigned char byte : digest) {
    out.push_back(hex[byte >> 4]);
    out.push_back(hex[byte & 0x0F]);
  }
  return out;
}

int maxImagesPerReceipt() {
  return std::stoi(receipt_scanner::config::envOr("MAX_IMAGES_PER_RECEIPT", "8"));
}

std::string generateImageKey(int64_t userId, int64_t receiptId) {
  return "receipts/" + std::to_string(userId) + "/" + std::to_string(receiptId) + "/" +
         auth::TokenUtils::generateToken(16) + ".webp";
}

std::optional<std::string> optionalString(const drogon::orm::Row &row, const std::string &field) {
  return row[field].isNull() ? std::nullopt : std::optional<std::string>(row[field].as<std::string>());
}

ReceiptSummary rowToSummary(const drogon::orm::Row &row) {
  return ReceiptSummary{
      row["id"].as<int64_t>(),
      optionalString(row, "store_name_raw"),
      optionalString(row, "merchant"),
      optionalString(row, "purchase_date"),
      row["total_cents"].as<int64_t>(),
      row["direction"].as<std::string>(),
      row["currency"].as<std::string>(),
      row["kind"].as<std::string>(),
      row["scan_state"].as<std::string>(),
      row["tax_deductible"].as<bool>(),
      optionalString(row, "note"),
      row["source"].as<std::string>(),
  };
}

} // namespace

drogon::Task<std::variant<std::vector<ReceiptService::PreparedImage>, ImageConflict>> ReceiptService::prepareImages(
    int64_t userId, const std::vector<std::string> &rawImages, bool force) {
  std::vector<PreparedImage> prepared;
  prepared.reserve(rawImages.size());

  for (const auto &raw : rawImages) {
    // Throws (undecodable bytes -- not JPEG/PNG/WebP, e.g. HEIC, which the pinned libvips
    // build has no decoder for) propagates straight out of this coroutine to the caller;
    // the controller translates it to a 400.
    auto normalized = co_await normalizer_.normalize(raw);
    auto hash = sha256Hex(normalized.bytes);

    if (!force) {
      auto dup = co_await db_->execSqlCoro(
          "SELECT id, receipt_id FROM receipt_images WHERE user_id = $1 AND image_sha256 = $2 LIMIT 1", userId,
          hash);
      if (!dup.empty()) {
        co_return ImageConflict{dup[0]["receipt_id"].as<int64_t>(), dup[0]["id"].as<int64_t>()};
      }
    }

    prepared.push_back(PreparedImage{std::move(normalized), std::move(hash)});
  }

  co_return prepared;
}

drogon::Task<std::variant<int64_t, ImageConflict>> ReceiptService::uploadReceipt(int64_t userId,
                                                                                  std::vector<std::string> images,
                                                                                  bool force) {
  auto prepared = co_await prepareImages(userId, images, force);
  if (std::holds_alternative<ImageConflict>(prepared)) {
    co_return std::get<ImageConflict>(prepared);
  }

  auto transaction = co_await db_->newTransactionCoro();
  // Provisional kind/direction (spec: "receipts.kind: purchase (default)") -- Step 10's
  // scanner overwrites these once it classifies the actual document type.
  auto receiptResult = co_await transaction->execSqlCoro(
      "INSERT INTO receipts (user_id, kind, direction, source, scan_state) "
      "VALUES ($1, 'purchase', 'outflow', 'scanned', 'uploaded') RETURNING id",
      userId);
  auto receiptId = receiptResult[0]["id"].as<int64_t>();

  // S3 writes happen here, inside the open (uncommitted) Postgres transaction -- the two
  // systems aren't transactionally joined. Deliberate ordering: S3 PUT before the DB insert
  // that references it, so a failure leaves at worst an orphaned S3 object (wasted storage,
  // no dangling DB reference) rather than a DB row pointing at an image that was never
  // actually written.
  for (auto &prep : std::get<std::vector<PreparedImage>>(prepared)) {
    auto key = generateImageKey(userId, receiptId);
    co_await imageStore_.put(key, "image/webp", prep.normalized.bytes);
    co_await transaction->execSqlCoro(
        "INSERT INTO receipt_images (receipt_id, user_id, image_key, image_sha256) VALUES ($1, $2, $3, $4)",
        receiptId, userId, key, prep.sha256Hex);
  }

  co_return receiptId;
}

drogon::Task<std::variant<std::monostate, ImageConflict>> ReceiptService::addImages(int64_t userId,
                                                                                     int64_t receiptId,
                                                                                     std::vector<std::string> images,
                                                                                     bool force) {
  auto owned = co_await db_->execSqlCoro("SELECT 1 FROM receipts WHERE id = $1 AND user_id = $2", receiptId, userId);
  if (owned.empty()) {
    throw ReceiptNotFoundError();
  }

  auto countResult =
      co_await db_->execSqlCoro("SELECT count(*) AS n FROM receipt_images WHERE receipt_id = $1", receiptId);
  auto currentCount = countResult[0]["n"].as<int64_t>();
  if (currentCount + static_cast<int64_t>(images.size()) > maxImagesPerReceipt()) {
    throw TooManyImagesError();
  }

  auto prepared = co_await prepareImages(userId, images, force);
  if (std::holds_alternative<ImageConflict>(prepared)) {
    co_return std::get<ImageConflict>(prepared);
  }

  auto transaction = co_await db_->newTransactionCoro();
  for (auto &prep : std::get<std::vector<PreparedImage>>(prepared)) {
    auto key = generateImageKey(userId, receiptId);
    co_await imageStore_.put(key, "image/webp", prep.normalized.bytes);
    co_await transaction->execSqlCoro(
        "INSERT INTO receipt_images (receipt_id, user_id, image_key, image_sha256) VALUES ($1, $2, $3, $4)",
        receiptId, userId, key, prep.sha256Hex);
  }
  // Sets the receipt up for the Step 10 worker to (re-)scan the FULL image set once it
  // exists; until then this just parks at scan_state=uploaded, per spec (explicitly
  // expected: "until Step 10 lands it just parks there").
  co_await transaction->execSqlCoro("UPDATE receipts SET scan_state = 'uploaded' WHERE id = $1", receiptId);

  co_return std::monostate{};
}

drogon::Task<std::optional<ReceiptSummary>> ReceiptService::getReceipt(int64_t userId, int64_t receiptId) {
  auto result = co_await db_->execSqlCoro("SELECT * FROM receipts WHERE id = $1 AND user_id = $2", receiptId, userId);
  if (result.empty()) {
    co_return std::nullopt;
  }
  co_return rowToSummary(result[0]);
}

drogon::Task<std::vector<ReceiptSummary>> ReceiptService::listReceipts(int64_t userId, int limit, int offset) {
  auto result = co_await db_->execSqlCoro(
      // Bare LIMIT/OFFSET placeholders have no column to infer a type from, so Postgres
      // defaults them to bigint -- explicit ::int casts keep that in sync with the int
      // (4-byte) binary params Drogon actually binds here, avoiding a param-size mismatch.
      "SELECT * FROM receipts WHERE user_id = $1 ORDER BY created_at DESC, id DESC LIMIT $2::int OFFSET $3::int",
      userId, limit, offset);
  std::vector<ReceiptSummary> out;
  out.reserve(result.size());
  for (const auto &row : result) {
    out.push_back(rowToSummary(row));
  }
  co_return out;
}

drogon::Task<std::vector<ImageRef>> ReceiptService::listImages(int64_t userId, int64_t receiptId) {
  auto owned = co_await db_->execSqlCoro("SELECT 1 FROM receipts WHERE id = $1 AND user_id = $2", receiptId, userId);
  if (owned.empty()) {
    throw ReceiptNotFoundError();
  }

  auto result = co_await db_->execSqlCoro(
      "SELECT id, created_at FROM receipt_images WHERE receipt_id = $1 ORDER BY created_at", receiptId);
  std::vector<ImageRef> out;
  out.reserve(result.size());
  for (const auto &row : result) {
    out.push_back(ImageRef{row["id"].as<int64_t>(), row["created_at"].as<std::string>()});
  }
  co_return out;
}

drogon::Task<bool> ReceiptService::deleteReceipt(int64_t userId, int64_t receiptId) {
  // Ownership-scoped from the start (the JOIN), so this never reads another user's image
  // keys even transiently -- not just the final DELETE below.
  auto images = co_await db_->execSqlCoro(
      "SELECT ri.image_key FROM receipt_images ri JOIN receipts r ON r.id = ri.receipt_id "
      "WHERE ri.receipt_id = $1 AND r.user_id = $2",
      receiptId, userId);

  auto result =
      co_await db_->execSqlCoro("DELETE FROM receipts WHERE id = $1 AND user_id = $2 RETURNING id", receiptId, userId);
  if (result.empty()) {
    co_return false;
  }

  for (const auto &row : images) {
    try {
      co_await imageStore_.remove(row["image_key"].as<std::string>());
    } catch (const std::exception &e) {
      LOG_WARN << "ReceiptService::deleteReceipt: failed to remove S3 object " << row["image_key"].as<std::string>()
               << " after the DB row was already deleted: " << e.what();
    }
  }

  co_return true;
}

drogon::Task<std::optional<ImageLocation>> ReceiptService::findImage(int64_t imageId) {
  auto result = co_await db_->execSqlCoro(
      "SELECT ri.image_key, r.user_id FROM receipt_images ri JOIN receipts r ON r.id = ri.receipt_id "
      "WHERE ri.id = $1",
      imageId);
  if (result.empty()) {
    co_return std::nullopt;
  }
  co_return ImageLocation{result[0]["user_id"].as<int64_t>(), result[0]["image_key"].as<std::string>()};
}

} // namespace receipt_scanner::receipts
