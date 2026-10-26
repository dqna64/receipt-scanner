#include "auth/SodiumInit.h"
#include "config/Env.h"
#include "scan/ScanServices.h"
#include "storage/VipsInit.h"

#include <drogon/drogon.h>
#include <drogon/orm/DbConfig.h>

#include <cstdio>

// HealthController/AuthController (METHOD_LIST_BEGIN/END) self-register via static
// initialization in their own translation units — see CMakeLists.txt for why
// receipt_scanner_lib is an OBJECT library (a STATIC library would let the linker drop
// those unreferenced .o files entirely).

int main() {
  using receipt_scanner::config::envOr;

  // stdout is fully-buffered (not line-buffered) whenever it isn't a TTY -- i.e. always,
  // once this runs under Docker/CI/nohup. Without this, log lines (including the
  // EMAIL_ENABLED=false verification/reset token logs) can sit invisible in a buffer
  // indefinitely instead of reaching `docker logs`/a redirected file promptly.
  setvbuf(stdout, nullptr, _IOLBF, 0);

  receipt_scanner::auth::ensureSodiumInitialized();
  receipt_scanner::storage::initVips("receipt_scanner");

  // A receipt can carry up to MAX_IMAGES_PER_RECEIPT images (default 8) at 15MB
  // each (spec Upload constraints) -- generous headroom over that worst case for multipart
  // framing overhead. Caddy enforces its own copy of the per-file limit in front of this
  // (docker-compose); this is the app-layer backstop.
  drogon::app().setClientMaxBodySize(130 * 1024 * 1024);

  drogon::orm::PostgresConfig dbConfig{
      .host = envOr("DB_HOST", "localhost"),
      .port = static_cast<unsigned short>(std::stoi(envOr("DB_PORT", "5432"))),
      .databaseName = envOr("DB_NAME", "receipt_scanner"),
      .username = envOr("DB_USER", "receipt_scanner"),
      .password = envOr("DB_PASSWORD", ""),
      .connectionNumber = 4,
      .name = "default",
      .isFast = false,
      .characterSet = "",
      .timeout = -1.0,
      .autoBatch = false,
      .connectOptions = {},
  };
  drogon::app().addDbClient(dbConfig);

  drogon::app().addListener("0.0.0.0", 8080).setThreadNum(4);
  // The scan worker's poll timer needs the running main loop (and the DB client and
  // S3 store, which come up with it), hence a beginning advice rather than starting it here.
  drogon::app().registerBeginningAdvice([] { receipt_scanner::scan::startScanWorker(); });
  drogon::app().run();
  return 0;
}
