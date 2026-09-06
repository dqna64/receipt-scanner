#pragma once

#include <drogon/utils/coroutine.h>

#include <string>

namespace receipt_scanner::storage {

// Storage backend for receipt images. One interface, swappable implementations (spec
// Architecture, cloud-portability requirement) -- S3ImageStore today (MinIO), potentially a
// local-filesystem implementation for tests later, without touching any caller.
class ImageStore {
public:
  virtual ~ImageStore() = default;

  virtual drogon::Task<> put(std::string key, std::string contentType, std::string bytes) = 0;
  virtual drogon::Task<std::string> get(std::string key) = 0;
  virtual drogon::Task<> remove(std::string key) = 0;
};

} // namespace receipt_scanner::storage
