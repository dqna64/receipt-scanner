#include "config/Env.h"

#include <drogon/drogon.h>
#include <drogon/orm/DbConfig.h>

// HealthController (METHOD_LIST_BEGIN/END) self-registers via static initialization in
// its own translation unit — see CMakeLists.txt for why receipt_scanner_lib is an OBJECT
// library (a STATIC library would let the linker drop that unreferenced .o entirely).

int main() {
  using receipt_scanner::config::envOr;

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
  drogon::app().run();
  return 0;
}
