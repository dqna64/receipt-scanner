#include "storage/ImageNormalizer.h"
#include "storage/VipsInit.h"

#include <catch2/catch_test_macros.hpp>
#include <drogon/utils/coroutine.h>
#include <vips/vips8>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <sstream>

using receipt_scanner::storage::ImageNormalizer;
using receipt_scanner::storage::NormalizedImage;

namespace {

std::string readFile(const std::string &path) {
  std::ifstream f(path, std::ios::binary);
  REQUIRE(f.is_open());
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

const std::string &fixturesDir() {
  // Fixtures live in the source tree, not the build dir (they're static test data,
  // generated once with Pillow -- see the generation notes in git history -- not produced
  // by the build). CMAKE_SOURCE_DIR is baked in via a compile definition (see
  // tests/CMakeLists.txt) so this works regardless of the build directory's location.
  static const std::string dir = std::string(RECEIPT_SCANNER_SOURCE_DIR) + "/tests/fixtures/";
  return dir;
}

NormalizedImage normalizeSyncForTest(const std::string &bytes) {
  ImageNormalizer normalizer;
  NormalizedImage result;
  drogon::sync_wait([&]() -> drogon::Task<> {
    result = co_await normalizer.normalize(bytes);
  }());
  return result;
}

// Reads a pixel's RGB from a decoded (uncompressed) VImage -- used to check that rotation
// actually happened (not just that dimensions swapped), by comparing actual pixel content
// against the independently-generated "expected" fixture.
std::vector<double> pixelAt(const vips::VImage &img, int x, int y) {
  return img.getpoint(x, y);
}

// vips_image_get_fields (C API -- not wrapped in the C++ VImage class) enumerates every
// metadata field name attached to the image; the caller owns the returned array and must
// free it with g_strfreev.
std::vector<std::string> metadataFieldNames(const vips::VImage &img) {
  gchar **fields = vips_image_get_fields(img.get_image());
  std::vector<std::string> out;
  for (gchar **f = fields; *f != nullptr; ++f) {
    out.emplace_back(*f);
  }
  g_strfreev(fields);
  return out;
}

} // namespace

TEST_CASE("ImageNormalizer: huge landscape JPEG downscales to 2048px long edge, strips GPS", "[image_normalizer]") {
  receipt_scanner::storage::initVips("test");
  auto bytes = readFile(fixturesDir() + "huge_landscape_gps.jpg");

  auto result = normalizeSyncForTest(bytes);

  // Source is 4000x3000 (landscape, 4:3) -- long edge (width) must become exactly 2048;
  // short edge scales proportionally (2048 * 3000/4000 = 1536).
  REQUIRE(result.width == 2048);
  REQUIRE(result.height == 1536);

  // Decode the normalized WebP back and confirm no EXIF/GPS survived the round-trip --
  // the actual "strip all metadata" assertion the spec calls for, not just a size check.
  auto decoded = vips::VImage::new_from_buffer(result.bytes.data(), result.bytes.size(), "");
  for (const auto &field : metadataFieldNames(decoded)) {
    INFO("surviving metadata field: " << field);
    std::string lower = field;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
    REQUIRE(lower.find("gps") == std::string::npos);
    REQUIRE(lower.find("exif") == std::string::npos);
  }
}

TEST_CASE("ImageNormalizer: EXIF orientation is applied, not just recorded", "[image_normalizer]") {
  receipt_scanner::storage::initVips("test");
  auto rotatedBytes = readFile(fixturesDir() + "rotated_orientation6.jpg");
  auto expectedBytes = readFile(fixturesDir() + "rotated_expected.png");

  auto result = normalizeSyncForTest(rotatedBytes);

  // The fixture's STORED pixel buffer is 800x600 (landscape) with orientation=6; the
  // CORRECT displayed image is 600x800 (portrait). If auto-rotation didn't happen, the
  // normalized output would be landscape-shaped instead.
  REQUIRE(result.width == 600);
  REQUIRE(result.height == 800);

  // Compare actual pixel content against the independently-saved "expected" fixture (a
  // plain PNG with no EXIF ambiguity -- ground truth for what the image should look like
  // once correctly oriented), not just dimensions -- a coincidental 600x800 from a bug
  // elsewhere wouldn't fool this.
  auto normalized = vips::VImage::new_from_buffer(result.bytes.data(), result.bytes.size(), "");
  auto expected = vips::VImage::new_from_buffer(expectedBytes.data(), expectedBytes.size(), "");

  for (auto [x, y] : {std::pair{10, 10}, std::pair{590, 10}, std::pair{10, 790}, std::pair{300, 400}}) {
    auto a = pixelAt(normalized, x, y);
    auto b = pixelAt(expected, x, y);
    // WebP q80 is lossy and JPEG source already lost some precision -- allow a tolerance
    // rather than requiring bit-exact pixels, which lossy re-encoding can never guarantee.
    for (int c = 0; c < 3; ++c) {
      REQUIRE(std::abs(a[c] - b[c]) < 20.0);
    }
  }
}

TEST_CASE("ImageNormalizer: tiny image is never upscaled", "[image_normalizer]") {
  receipt_scanner::storage::initVips("test");
  auto bytes = readFile(fixturesDir() + "tiny.png");

  auto result = normalizeSyncForTest(bytes);

  // Source is 32x24 -- well under the 2048px cap. Spec: downscale only, never upscale.
  REQUIRE(result.width == 32);
  REQUIRE(result.height == 24);
}

TEST_CASE("ImageNormalizer: WebP input decodes and re-encodes correctly", "[image_normalizer]") {
  receipt_scanner::storage::initVips("test");
  auto bytes = readFile(fixturesDir() + "small.webp");

  auto result = normalizeSyncForTest(bytes);

  REQUIRE(result.width == 200);
  REQUIRE(result.height == 150);
  // Output is itself decodable WebP.
  REQUIRE_NOTHROW(vips::VImage::new_from_buffer(result.bytes.data(), result.bytes.size(), ""));
}

TEST_CASE("ImageNormalizer: garbage input throws rather than crashing", "[image_normalizer]") {
  receipt_scanner::storage::initVips("test");
  bool threw = false;
  try {
    normalizeSyncForTest("this is not an image, just some plain bytes");
  } catch (const std::exception &) {
    threw = true;
  }
  REQUIRE(threw);
}
