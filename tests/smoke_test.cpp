#include <catch2/catch_test_macros.hpp>

#include "Engine.h"

TEST_CASE("engine version")
{
    REQUIRE(amp::engineVersion() == "0.1.0");
}
