cmake_minimum_required(VERSION 3.21)
project(receipt_scanner CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

add_compile_options(-Wall -Wextra -Werror)

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  # GCC 12 on x86_64 has a known false-positive -Wrestrict in libstdc++'s char_traits.h at
  # -O2 (an optimizer/value-range diagnostic bug, not a real issue in our code) -- observed
  # in CI (x86_64) but not on macOS/clang or arm64 Docker, both GCC-free or arm64-codegen.
  # Keep it a warning, not a hard error, rather than disabling -Werror project-wide.
  add_compile_options(-Wno-error=restrict)
endif()

find_package(Drogon CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(unofficial-sodium CONFIG REQUIRED)

# OBJECT (not STATIC): Drogon controllers self-register via static initializers in their
# own translation unit. A STATIC library's linker would drop an .o nothing else references
# (which is every controller, by design) instead of pulling it into the final binary.
add_library(receipt_scanner_lib OBJECT
  src/controllers/HealthController.cpp
  src/controllers/AuthController.cpp
  src/auth/SodiumInit.cpp
  src/auth/PasswordHasher.cpp
  src/auth/TokenUtils.cpp
  src/auth/AuthService.cpp
  src/filters/SessionAuthFilter.cpp
)
target_include_directories(receipt_scanner_lib PUBLIC src)
target_link_libraries(receipt_scanner_lib PUBLIC Drogon::Drogon nlohmann_json::nlohmann_json unofficial-sodium::sodium)

add_executable(receipt_scanner_server src/main.cpp)
target_link_libraries(receipt_scanner_server PRIVATE receipt_scanner_lib)

enable_testing()
add_subdirectory(tests)
