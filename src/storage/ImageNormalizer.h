#pragma once

#include <drogon/utils/coroutine.h>
#include <trantor/net/EventLoopThreadPool.h>

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>

namespace receipt_scanner::storage {

class ImageDecodeError : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

struct NormalizedImage {
  std::string bytes; // WebP-encoded
  int width;
  int height;
};

// Server-side canonical version regardless of what the client uploads (spec Object storage):
// decode -> apply EXIF orientation -> strip ALL metadata (phone photos embed GPS) -> downscale
// to 2048px long edge (never upscale) -> encode WebP q80. Library: libvips.
//
// CPU-bound rule (spec): normalization runs on a small worker thread pool, resuming the
// coroutine after -- NEVER on a Drogon IO loop. A single decode+resize+encode is genuinely
// slow (tens to low-hundreds of ms for a phone photo) -- blocking an IO loop thread for that
// long would stall every other request being served by that thread for the duration.
class ImageNormalizer {
public:
  static constexpr int kLongEdgePx = 2048;
  static constexpr int kWebpQuality = 80;

  explicit ImageNormalizer(size_t workerThreads = 2);
  ~ImageNormalizer();

  // Throws ImageDecodeError (via the coroutine machinery, from the worker thread) if `bytes`
  // isn't a decodable JPEG/PNG/WebP -- the client's fault, a 400. A WebP encode failure
  // throws plain std::runtime_error instead: that's our fault, a 500.
  drogon::Task<NormalizedImage> normalize(std::string bytes) const;

private:
  mutable trantor::EventLoopThreadPool pool_;
};

} // namespace receipt_scanner::storage
