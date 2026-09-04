#include <catch2/catch_test_macros.hpp>

TEST_CASE("sanity check", "[trivial]") {
  REQUIRE(1 + 1 == 2);
}
