find_package(Catch2 3 CONFIG REQUIRED)

add_executable(unit_tests
  trivial_test.cpp
  db_test.cpp
  auth_test.cpp
)
target_link_libraries(unit_tests PRIVATE receipt_scanner_lib Catch2::Catch2WithMain)

include(CTest)
include(Catch)
catch_discover_tests(unit_tests)
