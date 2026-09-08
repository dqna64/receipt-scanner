#include "VipsInit.h"

#include <vips/vips8>

#include <mutex>
#include <stdexcept>

namespace receipt_scanner::storage {

void initVips(const char *argv0) {
  static std::once_flag flag;
  std::call_once(flag, [argv0] {
    if (VIPS_INIT(argv0)) {
      throw std::runtime_error(std::string("VIPS_INIT failed: ") + vips_error_buffer());
    }
  });
}

} // namespace receipt_scanner::storage
