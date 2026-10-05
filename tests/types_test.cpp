#include <gtest/gtest.h>

#include "matchcore/types.hpp"

namespace matchcore {
namespace {

TEST(Price, StoresRawWireValue) {
  // ITCH Price (4): raw 1234500 == displayed 123.4500.
  constexpr Price p{1234500};
  EXPECT_EQ(p.raw(), 1234500u);
}

TEST(Price, OrdersByRawValue) {
  constexpr Price lo{100};
  constexpr Price hi{200};
  EXPECT_LT(lo, hi);
  EXPECT_GT(hi, lo);
  EXPECT_EQ(lo, Price{100});
}

TEST(Price, DefaultIsZero) {
  constexpr Price p;
  EXPECT_EQ(p.raw(), 0u);
}

TEST(Side, HasBuyAndSell) {
  EXPECT_NE(Side::kBuy, Side::kSell);
}

}  // namespace
}  // namespace matchcore
