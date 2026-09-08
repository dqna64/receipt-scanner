#pragma once

namespace receipt_scanner::storage {

// libvips requires VIPS_INIT() before any other call. Idempotent and thread-safe
// (std::call_once) -- same rationale as auth/SodiumInit.h: both main() and the test binary
// (which never runs main()) need to call this, and neither should have to coordinate with
// the other about who goes first.
void initVips(const char *argv0);

} // namespace receipt_scanner::storage
