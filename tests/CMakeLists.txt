find_package(Catch2 3 CONFIG REQUIRED)

add_executable(unit_tests
  trivial_test.cpp
  db_test.cpp
  auth_test.cpp
  sigv4_test.cpp
  image_store_test.cpp
  image_normalizer_test.cpp
)
target_link_libraries(unit_tests PRIVATE receipt_scanner_lib Catch2::Catch2WithMain)
# Fixture images live in the source tree (tests/fixtures/), not the build dir -- this lets
# the test binary find them regardless of where/how it was built (CI vs local, in/out-of-tree).
target_compile_definitions(unit_tests PRIVATE RECEIPT_SCANNER_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

include(CTest)
include(Catch)
catch_discover_tests(unit_tests)
