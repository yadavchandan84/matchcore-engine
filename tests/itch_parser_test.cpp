#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <ostream>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "itch_builder.hpp"
#include "matchcore/itch/parser.hpp"

namespace matchcore::itch {
namespace {

using test::MessageBuilder;

constexpr std::uint16_t kLocate = 42;
constexpr std::uint16_t kTracking = 7;
constexpr std::uint64_t kTimestamp = 34'200'000'000'123ULL;  // 09:30:00.000000123

/// Captures every delivered message of one type.
template <class M>
struct Capture {
  std::vector<M> seen;
  void on(const M& m) { seen.push_back(m); }
};

/// Records the type byte of every delivered message, whatever its type.
struct AnyHandler {
  std::vector<char> types;
  template <class M>
  void on(const M& /*msg*/) {
    types.push_back(M::kType);
  }
};

/// Handles nothing: every message is length-checked only.
struct NullHandler {};

/// Decode `body` into exactly one M, asserting success.
template <class M>
M decode_one(const MessageBuilder& b) {
  Capture<M> cap;
  EXPECT_EQ(parse_message(b.body(), cap), ParseError::kOk);
  EXPECT_EQ(cap.seen.size(), 1U);
  return cap.seen.empty() ? M{} : cap.seen.front();
}

void expect_header(const MessageHeader& h) {
  EXPECT_EQ(h.stock_locate, kLocate);
  EXPECT_EQ(h.tracking_number, kTracking);
  EXPECT_EQ(h.timestamp, kTimestamp);
}

// ---------------------------------------------------------------------------
// Primitives
// ---------------------------------------------------------------------------

TEST(ItchWire, BeUintDecodesBigEndian) {
  constexpr wire::BeUint<2> u16{{0x12, 0x34}};
  constexpr wire::BeUint<4> u32{{0xDE, 0xAD, 0xBE, 0xEF}};
  constexpr wire::BeUint<6> u48{{0x01, 0x02, 0x03, 0x04, 0x05, 0x06}};
  constexpr wire::BeUint<8> u64{{0xFF, 0xEE, 0xDD, 0xCC, 0xBB, 0xAA, 0x99, 0x88}};
  static_assert(u16.value() == 0x1234U);
  static_assert(u32.value() == 0xDEADBEEFU);
  static_assert(u48.value() == 0x010203040506ULL);
  static_assert(u64.value() == 0xFFEEDDCCBBAA9988ULL);
  EXPECT_EQ(u48.value(), 0x010203040506ULL);
}

TEST(ItchWire, MaxTimestampFitsIn48Bits) {
  constexpr wire::BeUint<6> max{{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}};
  static_assert(max.value() == 0xFFFF'FFFF'FFFFULL);
}

TEST(FixedString, ViewTrimsTrailingSpaces) {
  Symbol s{{'A', 'A', 'P', 'L', ' ', ' ', ' ', ' '}};
  EXPECT_EQ(s.view(), "AAPL");
  Symbol full{{'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H'}};
  EXPECT_EQ(full.view(), "ABCDEFGH");
  Symbol blank{{' ', ' ', ' ', ' ', ' ', ' ', ' ', ' '}};
  EXPECT_EQ(blank.view(), "");
}

// ---------------------------------------------------------------------------
// Dispatch: every ITCH 5.0 type routes to the right struct with the right size
// ---------------------------------------------------------------------------

struct TypeCase {
  char type;
  std::size_t size;
  std::size_t side_offset;  // 0 if the message has no Buy/Sell indicator
};

// Readable parameter display in test output / ctest names.
void PrintTo(const TypeCase& c, std::ostream* os) { *os << "'" << c.type << "' size " << c.size; }

// 'h' and 'H' are distinct types; keep generated test names unique.
std::string type_case_name(const ::testing::TestParamInfo<TypeCase>& param_info) {
  const char type = param_info.param.type;
  return std::string(type >= 'a' ? "lower_" : "upper_") + type;
}

class ItchDispatch : public ::testing::TestWithParam<TypeCase> {};

TEST_P(ItchDispatch, RoutesTypeByteToMatchingMessage) {
  const auto [type, size, side_offset] = GetParam();
  std::vector<std::byte> body(size, std::byte{0});
  body[0] = static_cast<std::byte>(type);
  if (side_offset != 0) {
    body[side_offset] = static_cast<std::byte>('B');
  }

  AnyHandler h;
  ASSERT_EQ(parse_message(body, h), ParseError::kOk);
  ASSERT_EQ(h.types.size(), 1U);
  EXPECT_EQ(h.types.front(), type);
}

TEST_P(ItchDispatch, RejectsBodyOneByteShortOrLong) {
  const auto [type, size, side_offset] = GetParam();
  AnyHandler h;

  std::vector<std::byte> shorter(size - 1, std::byte{0});
  shorter[0] = static_cast<std::byte>(type);
  EXPECT_EQ(parse_message(shorter, h), ParseError::kBadLength);

  std::vector<std::byte> longer(size + 1, std::byte{0});
  longer[0] = static_cast<std::byte>(type);
  EXPECT_EQ(parse_message(longer, h), ParseError::kBadLength);

  EXPECT_TRUE(h.types.empty());
}

INSTANTIATE_TEST_SUITE_P(
    AllMessageTypes, ItchDispatch,
    ::testing::Values(TypeCase{'S', 12, 0}, TypeCase{'R', 39, 0}, TypeCase{'H', 25, 0},
                      TypeCase{'Y', 20, 0}, TypeCase{'L', 26, 0}, TypeCase{'V', 35, 0},
                      TypeCase{'W', 12, 0}, TypeCase{'K', 28, 0}, TypeCase{'J', 35, 0},
                      TypeCase{'h', 21, 0}, TypeCase{'A', 36, 19}, TypeCase{'F', 40, 19},
                      TypeCase{'E', 31, 0}, TypeCase{'C', 36, 0}, TypeCase{'X', 23, 0},
                      TypeCase{'D', 19, 0}, TypeCase{'U', 35, 0}, TypeCase{'P', 44, 19},
                      TypeCase{'Q', 40, 0}, TypeCase{'B', 19, 0}, TypeCase{'I', 50, 0},
                      TypeCase{'N', 20, 0}, TypeCase{'O', 48, 0}),
    type_case_name);

// ---------------------------------------------------------------------------
// Field-level decoding, one message type at a time
// ---------------------------------------------------------------------------

TEST(ItchDecode, SystemEvent) {
  MessageBuilder b;
  b.header('S', kLocate, kTracking, kTimestamp).ch('O');
  const auto m = decode_one<SystemEvent>(b);
  expect_header(m.header);
  EXPECT_EQ(m.event_code, 'O');
}

TEST(ItchDecode, StockDirectory) {
  MessageBuilder b;
  b.header('R', kLocate, kTracking, kTimestamp)
      .alpha("AAPL", 8)
      .ch('Q')
      .ch('N')
      .u32(100)
      .ch('N')
      .ch('C')
      .alpha("Z", 2)
      .ch('P')
      .ch('N')
      .ch(' ')
      .ch('1')
      .ch('N')
      .u32(0)
      .ch('N');
  const auto m = decode_one<StockDirectory>(b);
  expect_header(m.header);
  EXPECT_EQ(m.stock.view(), "AAPL");
  EXPECT_EQ(m.market_category, 'Q');
  EXPECT_EQ(m.financial_status_indicator, 'N');
  EXPECT_EQ(m.round_lot_size, 100U);
  EXPECT_EQ(m.round_lots_only, 'N');
  EXPECT_EQ(m.issue_classification, 'C');
  EXPECT_EQ(m.issue_sub_type.view(), "Z");
  EXPECT_EQ(m.authenticity, 'P');
  EXPECT_EQ(m.short_sale_threshold_indicator, 'N');
  EXPECT_EQ(m.ipo_flag, ' ');
  EXPECT_EQ(m.luld_reference_price_tier, '1');
  EXPECT_EQ(m.etp_flag, 'N');
  EXPECT_EQ(m.etp_leverage_factor, 0U);
  EXPECT_EQ(m.inverse_indicator, 'N');
}

TEST(ItchDecode, StockTradingAction) {
  MessageBuilder b;
  b.header('H', kLocate, kTracking, kTimestamp).alpha("MSFT", 8).ch('T').ch(' ').alpha("", 4);
  const auto m = decode_one<StockTradingAction>(b);
  expect_header(m.header);
  EXPECT_EQ(m.stock.view(), "MSFT");
  EXPECT_EQ(m.trading_state, 'T');
  EXPECT_EQ(m.reason.view(), "");
}

TEST(ItchDecode, RegShoRestriction) {
  MessageBuilder b;
  b.header('Y', kLocate, kTracking, kTimestamp).alpha("TSLA", 8).ch('1');
  const auto m = decode_one<RegShoRestriction>(b);
  EXPECT_EQ(m.stock.view(), "TSLA");
  EXPECT_EQ(m.reg_sho_action, '1');
}

TEST(ItchDecode, MarketParticipantPosition) {
  MessageBuilder b;
  b.header('L', kLocate, kTracking, kTimestamp).alpha("GSCO", 4).alpha("AAPL", 8).ch('Y').ch('N').ch(
      'A');
  const auto m = decode_one<MarketParticipantPosition>(b);
  EXPECT_EQ(m.mpid.view(), "GSCO");
  EXPECT_EQ(m.stock.view(), "AAPL");
  EXPECT_EQ(m.primary_market_maker, 'Y');
  EXPECT_EQ(m.market_maker_mode, 'N');
  EXPECT_EQ(m.market_participant_state, 'A');
}

TEST(ItchDecode, MwcbDeclineLevelUsesEightDecimalPrices) {
  MessageBuilder b;
  b.header('V', kLocate, kTracking, kTimestamp)
      .u64(3'000'00000000ULL)
      .u64(2'800'00000000ULL)
      .u64(2'600'00000000ULL);
  const auto m = decode_one<MwcbDeclineLevel>(b);
  EXPECT_EQ(m.level1, 3'000'00000000ULL);
  EXPECT_EQ(m.level2, 2'800'00000000ULL);
  EXPECT_EQ(m.level3, 2'600'00000000ULL);
}

TEST(ItchDecode, MwcbStatus) {
  MessageBuilder b;
  b.header('W', kLocate, kTracking, kTimestamp).ch('2');
  EXPECT_EQ(decode_one<MwcbStatus>(b).breached_level, '2');
}

TEST(ItchDecode, IpoQuotingPeriodUpdate) {
  MessageBuilder b;
  b.header('K', kLocate, kTracking, kTimestamp).alpha("NEWCO", 8).u32(36'000).ch('A').u32(250000);
  const auto m = decode_one<IpoQuotingPeriodUpdate>(b);
  EXPECT_EQ(m.stock.view(), "NEWCO");
  EXPECT_EQ(m.ipo_quotation_release_time, 36'000U);
  EXPECT_EQ(m.ipo_quotation_release_qualifier, 'A');
  EXPECT_EQ(m.ipo_price, Price{250000});
}

TEST(ItchDecode, LuldAuctionCollar) {
  MessageBuilder b;
  b.header('J', kLocate, kTracking, kTimestamp)
      .alpha("AMC", 8)
      .u32(100000)
      .u32(110000)
      .u32(90000)
      .u32(3);
  const auto m = decode_one<LuldAuctionCollar>(b);
  EXPECT_EQ(m.stock.view(), "AMC");
  EXPECT_EQ(m.auction_collar_reference_price, Price{100000});
  EXPECT_EQ(m.upper_auction_collar_price, Price{110000});
  EXPECT_EQ(m.lower_auction_collar_price, Price{90000});
  EXPECT_EQ(m.auction_collar_extension, 3U);
}

TEST(ItchDecode, OperationalHalt) {
  MessageBuilder b;
  b.header('h', kLocate, kTracking, kTimestamp).alpha("IBM", 8).ch('Q').ch('H');
  const auto m = decode_one<OperationalHalt>(b);
  EXPECT_EQ(m.stock.view(), "IBM");
  EXPECT_EQ(m.market_code, 'Q');
  EXPECT_EQ(m.operational_halt_action, 'H');
}

TEST(ItchDecode, AddOrder) {
  MessageBuilder b;
  b.header('A', kLocate, kTracking, kTimestamp)
      .u64(0x0102030405060708ULL)
      .ch('S')
      .u32(500)
      .alpha("AAPL", 8)
      .u32(1'893'400);  // 189.3400
  const auto m = decode_one<AddOrder>(b);
  expect_header(m.header);
  EXPECT_EQ(m.order_reference_number, 0x0102030405060708ULL);
  EXPECT_EQ(m.side, Side::kSell);
  EXPECT_EQ(m.shares, 500U);
  EXPECT_EQ(m.stock.view(), "AAPL");
  EXPECT_EQ(m.price, Price{1'893'400});
}

TEST(ItchDecode, AddOrderMpid) {
  MessageBuilder b;
  b.header('F', kLocate, kTracking, kTimestamp)
      .u64(99)
      .ch('B')
      .u32(100)
      .alpha("NVDA", 8)
      .u32(4'501'200)
      .alpha("MSCO", 4);
  const auto m = decode_one<AddOrderMpid>(b);
  EXPECT_EQ(m.order_reference_number, 99U);
  EXPECT_EQ(m.side, Side::kBuy);
  EXPECT_EQ(m.shares, 100U);
  EXPECT_EQ(m.stock.view(), "NVDA");
  EXPECT_EQ(m.price, Price{4'501'200});
  EXPECT_EQ(m.attribution.view(), "MSCO");
}

TEST(ItchDecode, OrderExecuted) {
  MessageBuilder b;
  b.header('E', kLocate, kTracking, kTimestamp).u64(1234).u32(75).u64(987654321);
  const auto m = decode_one<OrderExecuted>(b);
  expect_header(m.header);
  EXPECT_EQ(m.order_reference_number, 1234U);
  EXPECT_EQ(m.executed_shares, 75U);
  EXPECT_EQ(m.match_number, 987654321U);
}

TEST(ItchDecode, OrderExecutedWithPrice) {
  MessageBuilder b;
  b.header('C', kLocate, kTracking, kTimestamp).u64(1234).u32(75).u64(55).ch('Y').u32(1'000'100);
  const auto m = decode_one<OrderExecutedWithPrice>(b);
  EXPECT_EQ(m.order_reference_number, 1234U);
  EXPECT_EQ(m.executed_shares, 75U);
  EXPECT_EQ(m.match_number, 55U);
  EXPECT_EQ(m.printable, 'Y');
  EXPECT_EQ(m.execution_price, Price{1'000'100});
}

TEST(ItchDecode, OrderCancel) {
  MessageBuilder b;
  b.header('X', kLocate, kTracking, kTimestamp).u64(777).u32(25);
  const auto m = decode_one<OrderCancel>(b);
  EXPECT_EQ(m.order_reference_number, 777U);
  EXPECT_EQ(m.cancelled_shares, 25U);
}

TEST(ItchDecode, OrderDelete) {
  MessageBuilder b;
  b.header('D', kLocate, kTracking, kTimestamp).u64(0xFFFFFFFFFFFFFFFFULL);
  EXPECT_EQ(decode_one<OrderDelete>(b).order_reference_number, 0xFFFFFFFFFFFFFFFFULL);
}

TEST(ItchDecode, OrderReplace) {
  MessageBuilder b;
  b.header('U', kLocate, kTracking, kTimestamp).u64(10).u64(11).u32(300).u32(123'4500);
  const auto m = decode_one<OrderReplace>(b);
  EXPECT_EQ(m.original_order_reference_number, 10U);
  EXPECT_EQ(m.new_order_reference_number, 11U);
  EXPECT_EQ(m.shares, 300U);
  EXPECT_EQ(m.price, Price{123'4500});
}

TEST(ItchDecode, Trade) {
  MessageBuilder b;
  b.header('P', kLocate, kTracking, kTimestamp)
      .u64(0)
      .ch('B')
      .u32(200)
      .alpha("SPY", 8)
      .u32(4'300'000)
      .u64(4242);
  const auto m = decode_one<Trade>(b);
  EXPECT_EQ(m.order_reference_number, 0U);
  EXPECT_EQ(m.side, Side::kBuy);
  EXPECT_EQ(m.shares, 200U);
  EXPECT_EQ(m.stock.view(), "SPY");
  EXPECT_EQ(m.price, Price{4'300'000});
  EXPECT_EQ(m.match_number, 4242U);
}

TEST(ItchDecode, CrossTradeHasEightByteShares) {
  MessageBuilder b;
  b.header('Q', kLocate, kTracking, kTimestamp)
      .u64(5'000'000'000ULL)  // > 2^32: must not be truncated
      .alpha("QQQ", 8)
      .u32(3'500'000)
      .u64(77)
      .ch('O');
  const auto m = decode_one<CrossTrade>(b);
  EXPECT_EQ(m.shares, 5'000'000'000ULL);
  EXPECT_EQ(m.stock.view(), "QQQ");
  EXPECT_EQ(m.cross_price, Price{3'500'000});
  EXPECT_EQ(m.match_number, 77U);
  EXPECT_EQ(m.cross_type, 'O');
}

TEST(ItchDecode, BrokenTrade) {
  MessageBuilder b;
  b.header('B', kLocate, kTracking, kTimestamp).u64(31337);
  EXPECT_EQ(decode_one<BrokenTrade>(b).match_number, 31337U);
}

TEST(ItchDecode, Noii) {
  MessageBuilder b;
  b.header('I', kLocate, kTracking, kTimestamp)
      .u64(10'000)
      .u64(2'500)
      .ch('B')
      .alpha("AAPL", 8)
      .u32(1'900'000)
      .u32(1'895'000)
      .u32(1'893'400)
      .ch('C')
      .ch('L');
  const auto m = decode_one<Noii>(b);
  EXPECT_EQ(m.paired_shares, 10'000U);
  EXPECT_EQ(m.imbalance_shares, 2'500U);
  EXPECT_EQ(m.imbalance_direction, 'B');
  EXPECT_EQ(m.stock.view(), "AAPL");
  EXPECT_EQ(m.far_price, Price{1'900'000});
  EXPECT_EQ(m.near_price, Price{1'895'000});
  EXPECT_EQ(m.current_reference_price, Price{1'893'400});
  EXPECT_EQ(m.cross_type, 'C');
  EXPECT_EQ(m.price_variation_indicator, 'L');
}

TEST(ItchDecode, RetailPriceImprovement) {
  MessageBuilder b;
  b.header('N', kLocate, kTracking, kTimestamp).alpha("GE", 8).ch('A');
  const auto m = decode_one<RetailPriceImprovement>(b);
  EXPECT_EQ(m.stock.view(), "GE");
  EXPECT_EQ(m.interest_flag, 'A');
}

TEST(ItchDecode, DirectListingCapitalRaise) {
  MessageBuilder b;
  b.header('O', kLocate, kTracking, kTimestamp)
      .alpha("DLST", 8)
      .ch('Y')
      .u32(80'000)
      .u32(120'000)
      .u32(100'000)
      .u64(kTimestamp + 1)
      .u32(85'000)
      .u32(115'000);
  const auto m = decode_one<DirectListingCapitalRaise>(b);
  EXPECT_EQ(m.stock.view(), "DLST");
  EXPECT_EQ(m.open_eligibility_status, 'Y');
  EXPECT_EQ(m.minimum_allowable_price, Price{80'000});
  EXPECT_EQ(m.maximum_allowable_price, Price{120'000});
  EXPECT_EQ(m.near_execution_price, Price{100'000});
  EXPECT_EQ(m.near_execution_time, kTimestamp + 1);
  EXPECT_EQ(m.lower_price_range_collar, Price{85'000});
  EXPECT_EQ(m.upper_price_range_collar, Price{115'000});
}

// ---------------------------------------------------------------------------
// Error handling
// ---------------------------------------------------------------------------

MessageBuilder add_order_with_side(char side) {
  MessageBuilder b;
  b.header('A', kLocate, kTracking, kTimestamp).u64(1).ch(side).u32(1).alpha("X", 8).u32(1);
  return b;
}

TEST(ItchErrors, EmptyBody) {
  AnyHandler h;
  EXPECT_EQ(parse_message(std::span<const std::byte>{}, h), ParseError::kEmpty);
}

TEST(ItchErrors, UnknownType) {
  const std::vector<std::byte> body(20, std::byte{'Z'});
  AnyHandler h;
  EXPECT_EQ(parse_message(body, h), ParseError::kUnknownType);
  EXPECT_TRUE(h.types.empty());
}

TEST(ItchErrors, InvalidSideIsRejectedAndNotDelivered) {
  Capture<AddOrder> cap;
  EXPECT_EQ(parse_message(add_order_with_side('?').body(), cap), ParseError::kInvalidField);
  EXPECT_TRUE(cap.seen.empty());
}

TEST(ItchErrors, UnhandledTypesAreOnlyLengthChecked) {
  // A handler with no AddOrder overload never pays for decoding it, so the
  // bad side byte is not inspected.
  NullHandler h;
  EXPECT_EQ(parse_message(add_order_with_side('?').body(), h), ParseError::kOk);
}

TEST(ItchErrors, ToStringCoversAllValues) {
  EXPECT_EQ(to_string(ParseError::kOk), "ok");
  EXPECT_FALSE(to_string(ParseError::kEmpty).empty());
  EXPECT_FALSE(to_string(ParseError::kUnknownType).empty());
  EXPECT_FALSE(to_string(ParseError::kBadLength).empty());
  EXPECT_FALSE(to_string(ParseError::kInvalidField).empty());
}

TEST(ItchErrors, HandlerExceptionsPropagate) {
  struct Throwing {
    void on(const OrderDelete& /*m*/) { throw std::runtime_error("boom"); }
  };
  MessageBuilder b;
  b.header('D', kLocate, kTracking, kTimestamp).u64(1);
  Throwing h;
  EXPECT_THROW((void)parse_message(b.body(), h), std::runtime_error);
}

// ---------------------------------------------------------------------------
// Stream framing
// ---------------------------------------------------------------------------

/// A small mixed session: S, R, A, E, X, U, D, P.
std::vector<std::byte> sample_stream() {
  std::vector<std::byte> s;
  MessageBuilder m;

  m = {};
  m.header('S', 0, 0, 1).ch('O');
  test::append(s, m);

  m = {};
  m.header('R', 1, 0, 2)
      .alpha("AAPL", 8)
      .ch('Q')
      .ch('N')
      .u32(100)
      .ch('N')
      .ch('C')
      .alpha("", 2)
      .ch('P')
      .ch('N')
      .ch(' ')
      .ch('1')
      .ch('N')
      .u32(0)
      .ch('N');
  test::append(s, m);

  m = {};
  m.header('A', 1, 1, 3).u64(100).ch('B').u32(500).alpha("AAPL", 8).u32(1'893'400);
  test::append(s, m);

  m = {};
  m.header('E', 1, 1, 4).u64(100).u32(200).u64(1);
  test::append(s, m);

  m = {};
  m.header('X', 1, 1, 5).u64(100).u32(100);
  test::append(s, m);

  m = {};
  m.header('U', 1, 1, 6).u64(100).u64(101).u32(150).u32(1'893'500);
  test::append(s, m);

  m = {};
  m.header('D', 1, 1, 7).u64(101);
  test::append(s, m);

  m = {};
  m.header('P', 1, 1, 8).u64(0).ch('B').u32(10).alpha("AAPL", 8).u32(1'893'000).u64(2);
  test::append(s, m);

  return s;
}

const std::vector<char> kSampleTypes{'S', 'R', 'A', 'E', 'X', 'U', 'D', 'P'};

TEST(ItchStream, ParsesEveryFrameInOrder) {
  const auto s = sample_stream();
  AnyHandler h;
  const auto r = parse_stream(s, h);
  EXPECT_EQ(r.bytes_consumed, s.size());
  EXPECT_EQ(r.messages, kSampleTypes.size());
  EXPECT_EQ(r.unknown_types, 0U);
  EXPECT_EQ(r.malformed, 0U);
  EXPECT_EQ(h.types, kSampleTypes);
}

TEST(ItchStream, EmptyInput) {
  AnyHandler h;
  const auto r = parse_stream(std::span<const std::byte>{}, h);
  EXPECT_EQ(r.bytes_consumed, 0U);
  EXPECT_EQ(r.messages, 0U);
}

TEST(ItchStream, StopsBeforeIncompleteTrailingFrame) {
  auto s = sample_stream();
  const std::size_t complete = s.size();

  // A partial length prefix (1 byte) ...
  s.push_back(std::byte{0x00});
  AnyHandler h1;
  EXPECT_EQ(parse_stream(s, h1).bytes_consumed, complete);

  // ... and a full prefix with a partial body.
  s.push_back(std::byte{19});
  s.push_back(std::byte{'D'});
  AnyHandler h2;
  const auto r = parse_stream(s, h2);
  EXPECT_EQ(r.bytes_consumed, complete);
  EXPECT_EQ(h2.types, kSampleTypes);
}

TEST(ItchStream, SkipsUnknownAndMalformedFramesButKeepsGoing) {
  std::vector<std::byte> s;
  MessageBuilder unknown;
  unknown.ch('Z').u32(0);  // unknown type
  MessageBuilder bad_len;
  bad_len.header('D', 1, 1, 1);  // 'D' without its order reference
  MessageBuilder good;
  good.header('D', 1, 1, 2).u64(5);

  test::append(s, unknown);
  test::append(s, bad_len);
  s.push_back(std::byte{0});  // zero-length frame
  s.push_back(std::byte{0});
  test::append(s, good);

  Capture<OrderDelete> cap;
  const auto r = parse_stream(s, cap);
  EXPECT_EQ(r.bytes_consumed, s.size());
  EXPECT_EQ(r.messages, 1U);
  EXPECT_EQ(r.unknown_types, 1U);
  EXPECT_EQ(r.malformed, 2U);
  ASSERT_EQ(cap.seen.size(), 1U);
  EXPECT_EQ(cap.seen.front().order_reference_number, 5U);
}

TEST(ItchStream, ChunkedParsingMatchesWholeBufferAtEverySplitPoint) {
  // Simulates a chunked file reader: parse the first `split` bytes, carry the
  // unconsumed tail over, append the rest, parse again. Every split point must
  // yield exactly the same messages as parsing the whole buffer at once.
  const auto s = sample_stream();
  for (std::size_t split = 0; split <= s.size(); ++split) {
    AnyHandler h;
    const std::span<const std::byte> all{s};

    const auto first = parse_stream(all.first(split), h);
    std::vector<std::byte> carry(all.begin() + static_cast<std::ptrdiff_t>(first.bytes_consumed),
                                 all.end());
    const auto second = parse_stream(carry, h);

    EXPECT_EQ(first.bytes_consumed + second.bytes_consumed, s.size()) << "split=" << split;
    EXPECT_EQ(first.messages + second.messages, kSampleTypes.size()) << "split=" << split;
    EXPECT_EQ(h.types, kSampleTypes) << "split=" << split;
  }
}

}  // namespace
}  // namespace matchcore::itch
