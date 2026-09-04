cmake_minimum_required(VERSION 3.21)
project(receipt_scanner CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

add_compile_options(-Wall -Wextra -Werror)

find_package(Drogon CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

# OBJECT (not STATIC): Drogon controllers self-register via static initializers in their
# own translation unit. A STATIC library's linker would drop an .o nothing else references
# (which is every controller, by design) instead of pulling it into the final binary.
add_library(receipt_scanner_lib OBJECT
  src/controllers/HealthController.cpp
)
target_include_directories(receipt_scanner_lib PUBLIC src)
target_link_libraries(receipt_scanner_lib PUBLIC Drogon::Drogon nlohmann_json::nlohmann_json)

add_executable(receipt_scanner_server src/main.cpp)
target_link_libraries(receipt_scanner_server PRIVATE receipt_scanner_lib)

enable_testing()
add_subdirectory(tests)
