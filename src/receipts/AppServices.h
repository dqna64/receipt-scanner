#pragma once

#include "storage/ImageNormalizer.h"
#include "storage/S3ImageStore.h"

namespace receipt_scanner::receipts {

// Lazy singletons, same ambient-lookup pattern as drogon::app().getDbClient() (see
// AuthController) rather than constructor-injecting a controller: Drogon auto-creates
// controllers with a default constructor, so there's no clean place to pass these in short
// of abandoning isAutoCreation, which would make this controller diverge from every other
// one in the codebase for no real benefit. Constructed on first use (the first request that
// needs them), by which point drogon::app().run() has already started -- S3ImageStore's
// HttpClient needs a running trantor::EventLoop (see Step 6's S3ImageStore ctor comment).
storage::ImageNormalizer &imageNormalizer();
storage::S3ImageStore &imageStore();

} // namespace receipt_scanner::receipts
