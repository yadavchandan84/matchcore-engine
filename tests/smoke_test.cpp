#include <gtest/gtest.h>

#include "matchcore/version.hpp"

// Phase 1 smoke test: proves the library links and the toolchain/CI pipeline
// runs end to end. Real parser/book/engine tests arrive in later phases.
TEST(Smoke, VersionIsReported) {
  EXPECT_EQ(matchcore::version(), "0.1.0");
}

TEST(Smoke, VersionConstantsMatchString) {
  EXPECT_EQ(matchcore::kVersionMajor, 0u);
  EXPECT_EQ(matchcore::kVersionMinor, 1u);
  EXPECT_EQ(matchcore::kVersionPatch, 0u);
}
