#pragma once

namespace receipt_scanner::auth {

// libsodium requires sodium_init() before any other call. Idempotent and thread-safe
// (std::call_once) so every entry point that touches crypto -- main(), and the test binary,
// which never runs main() -- can just call this instead of relying on someone else having
// done it first.
void ensureSodiumInitialized();

} // namespace receipt_scanner::auth
