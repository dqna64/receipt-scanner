#include <drogon/drogon.h>

// HealthController (METHOD_LIST_BEGIN/END) self-registers via static initialization in
// its own translation unit — see CMakeLists.txt for why receipt_scanner_lib is an OBJECT
// library (a STATIC library would let the linker drop that unreferenced .o entirely).

int main() {
  drogon::app().addListener("0.0.0.0", 8080).setThreadNum(4);
  drogon::app().run();
  return 0;
}
