#include "SodiumInit.h"

#include <sodium.h>

#include <mutex>
#include <stdexcept>

namespace receipt_scanner::auth {

void ensureSodiumInitialized() {
  static std::once_flag flag;
  std::call_once(flag, [] {
    if (sodium_init() < 0) {
      throw std::runtime_error("sodium_init failed");
    }
  });
}

} // namespace receipt_scanner::auth
