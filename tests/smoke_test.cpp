#include <gtest/gtest.h>

#include "matchcore/version.hpp"

// Smoke test: proves the library links and the toolchain/CI pipeline runs end
// to end.
TEST(Smoke, VersionIsReported) {
  EXPECT_EQ(matchcore::version(), "1.0.0");
}

TEST(Smoke, VersionConstantsMatchString) {
  EXPECT_EQ(matchcore::kVersionMajor, 1u);
  EXPECT_EQ(matchcore::kVersionMinor, 0u);
  EXPECT_EQ(matchcore::kVersionPatch, 0u);
}
