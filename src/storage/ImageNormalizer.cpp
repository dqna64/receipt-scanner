#include "ImageNormalizer.h"

#include "VipsInit.h"

#include <trantor/net/EventLoop.h>
#include <vips/vips8>

#include <stdexcept>

namespace receipt_scanner::storage {

namespace {

NormalizedImage normalizeSync(const std::string &bytes) {
  initVips("receipt_scanner"); // idempotent -- see VipsInit.h

  vips::VImage image;
  try {
    // thumbnail_buffer: decode + shrink-on-load in one step (avoids decoding a full-res
    // phone photo just to immediately throw most of it away -- meaningfully faster than
    // load-then-resize for the sizes involved here). Fitting within a
    // kLongEdgePx x kLongEdgePx box, aspect preserved, is exactly "downscale to 2048px long
    // edge": whichever side is longer becomes <= 2048, the other scales proportionally.
    // VIPS_SIZE_DOWN is required explicitly -- the default lets it upscale small images,
    // which spec forbids (only ever downscale). EXIF-orientation auto-rotation is
    // vips_thumbnail's default behavior (the "no_rotate" option defaults to false) -- not
    // passed explicitly, relying on the documented default rather than guessing an option
    // name that isn't load-bearing to get right.
    //
    // thumbnail_buffer's C++ signature takes a non-const void* (the underlying C API never
    // writes through it -- just an incomplete const-correctness pass in the wrapper), hence
    // the const_cast.
    image = vips::VImage::thumbnail_buffer(const_cast<void *>(static_cast<const void *>(bytes.data())), bytes.size(),
                                            ImageNormalizer::kLongEdgePx,
                                            vips::VImage::option()
                                                ->set("height", ImageNormalizer::kLongEdgePx)
                                                ->set("size", VIPS_SIZE_DOWN));
  } catch (const vips::VError &e) {
    throw std::runtime_error(std::string("ImageNormalizer: failed to decode/resize image: ") + e.what());
  }

  try {
    // "strip": true removes ALL metadata on save -- EXIF (incl. GPS), ICC, XMP, everything.
    // This is the actual metadata-removal step; the thumbnail step above already consumed
    // the orientation tag's meaning (auto-rotated the pixels) before this discards it.
    //
    // webpsave_buffer returns a VipsBlob* (a refcounted GObject boxed type), not std::string
    // -- vips_blob_get() extracts the raw data pointer + length, which we copy into a
    // std::string we own, then the blob itself must be explicitly unref'd (it isn't RAII'd
    // by the C++ wrapper).
    VipsBlob *blob =
        image.webpsave_buffer(vips::VImage::option()->set("Q", ImageNormalizer::kWebpQuality)->set("strip", true));
    size_t len = 0;
    const void *data = vips_blob_get(blob, &len);
    std::string webpBytes(static_cast<const char *>(data), len);
    vips_area_unref(VIPS_AREA(blob));

    return NormalizedImage{std::move(webpBytes), image.width(), image.height()};
  } catch (const vips::VError &e) {
    throw std::runtime_error(std::string("ImageNormalizer: failed to encode WebP: ") + e.what());
  }
}

} // namespace

ImageNormalizer::ImageNormalizer(size_t workerThreads) : pool_(workerThreads, "ImgNorm") {
  pool_.start();
}

// No explicit destructor: EventLoopThreadPool's own destructor (via its EventLoopThread
// members' destructors) stops each loop and joins its thread. pool_.wait() would be the
// wrong call here anyway -- it blocks until loops quit but never itself requests the quit,
// so calling it before anything has asked the loops to stop would just hang forever.
ImageNormalizer::~ImageNormalizer() = default;

drogon::Task<NormalizedImage> ImageNormalizer::normalize(std::string bytes) const {
  // Capture the calling coroutine's own loop (nullptr if none, e.g. a plain sync_wait test
  // harness thread with no running EventLoop -- queueInLoopCoro tolerates a null resumeLoop,
  // it just resumes on the worker loop directly in that case) BEFORE switching off it, so
  // the coroutine can be bounced back to the loop it started on rather than being stranded
  // on the worker thread for the rest of its lifetime.
  auto *callerLoop = trantor::EventLoop::getEventLoopOfCurrentThread();
  auto *workerLoop = pool_.getNextLoop();

  NormalizedImage result;
  co_await drogon::queueInLoopCoro(
      workerLoop, [&bytes, &result] { result = normalizeSync(bytes); }, callerLoop);

  co_return result;
}

} // namespace receipt_scanner::storage
