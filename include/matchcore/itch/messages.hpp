#ifndef MATCHCORE_ITCH_MESSAGES_HPP
#define MATCHCORE_ITCH_MESSAGES_HPP

#include <cstddef>
#include <cstdint>

#include "matchcore/itch/wire.hpp"
#include "matchcore/types.hpp"

// Decoded NASDAQ TotalView-ITCH 5.0 messages.
//
// These are what handlers receive: plain, host-endian value types built from
// the wire layouts in wire.hpp. Each struct carries:
//   - kType     : the ITCH message type byte
//   - Wire      : the matching wire layout
//   - kWireSize : the exact body size on the wire (excluding the length prefix)
//
// Prices keep their raw fixed-point representation (`Price` for Price (4),
// `std::uint64_t` for the 8-decimal Price (8) fields). Single-character code
// fields are passed through unchanged as `char` so that no information is
// lost and the hot path does not pay for validating fields it ignores. The
// one exception is Buy/Sell, which the book needs and is decoded into `Side`.
namespace matchcore::itch {

/// Fields common to every ITCH message.
struct MessageHeader {
  StockLocate stock_locate{};
  std::uint16_t tracking_number{};
  Timestamp timestamp{};  // nanoseconds since midnight

  friend constexpr bool operator==(const MessageHeader&, const MessageHeader&) noexcept = default;
};

#define MATCHCORE_ITCH_MESSAGE(TYPE_CHAR, WIRE)                   \
  static constexpr char kType = (TYPE_CHAR);                      \
  using Wire = wire::WIRE;                                        \
  static constexpr std::size_t kWireSize = sizeof(wire::WIRE);    \
  MessageHeader header

// ---------------------------------------------------------------------------
// System / administrative messages
// ---------------------------------------------------------------------------

struct SystemEvent {
  MATCHCORE_ITCH_MESSAGE('S', SystemEvent);
  char event_code{};  // 'O','S','Q','M','E','C'
};

struct StockDirectory {
  MATCHCORE_ITCH_MESSAGE('R', StockDirectory);
  Symbol stock;
  char market_category{};
  char financial_status_indicator{};
  std::uint32_t round_lot_size{};
  char round_lots_only{};
  char issue_classification{};
  FixedString<2> issue_sub_type;
  char authenticity{};
  char short_sale_threshold_indicator{};
  char ipo_flag{};
  char luld_reference_price_tier{};
  char etp_flag{};
  std::uint32_t etp_leverage_factor{};
  char inverse_indicator{};
};

struct StockTradingAction {
  MATCHCORE_ITCH_MESSAGE('H', StockTradingAction);
  Symbol stock;
  char trading_state{};  // 'H','P','Q','T'
  char reserved{};
  FixedString<4> reason;
};

struct RegShoRestriction {
  MATCHCORE_ITCH_MESSAGE('Y', RegShoRestriction);
  Symbol stock;
  char reg_sho_action{};
};

struct MarketParticipantPosition {
  MATCHCORE_ITCH_MESSAGE('L', MarketParticipantPosition);
  FixedString<4> mpid;
  Symbol stock;
  char primary_market_maker{};
  char market_maker_mode{};
  char market_participant_state{};
};

struct MwcbDeclineLevel {
  MATCHCORE_ITCH_MESSAGE('V', MwcbDeclineLevel);
  std::uint64_t level1{};  // Price (8): 8 implied decimals
  std::uint64_t level2{};
  std::uint64_t level3{};
};

struct MwcbStatus {
  MATCHCORE_ITCH_MESSAGE('W', MwcbStatus);
  char breached_level{};
};

struct IpoQuotingPeriodUpdate {
  MATCHCORE_ITCH_MESSAGE('K', IpoQuotingPeriodUpdate);
  Symbol stock;
  std::uint32_t ipo_quotation_release_time{};  // seconds since midnight
  char ipo_quotation_release_qualifier{};
  Price ipo_price;
};

struct LuldAuctionCollar {
  MATCHCORE_ITCH_MESSAGE('J', LuldAuctionCollar);
  Symbol stock;
  Price auction_collar_reference_price;
  Price upper_auction_collar_price;
  Price lower_auction_collar_price;
  std::uint32_t auction_collar_extension{};
};

struct OperationalHalt {
  MATCHCORE_ITCH_MESSAGE('h', OperationalHalt);
  Symbol stock;
  char market_code{};
  char operational_halt_action{};
};

// ---------------------------------------------------------------------------
// Order book messages
// ---------------------------------------------------------------------------

struct AddOrder {
  MATCHCORE_ITCH_MESSAGE('A', AddOrder);
  OrderId order_reference_number{};
  Side side{};
  Quantity shares{};
  Symbol stock;
  Price price;
};

struct AddOrderMpid {
  MATCHCORE_ITCH_MESSAGE('F', AddOrderMpid);
  OrderId order_reference_number{};
  Side side{};
  Quantity shares{};
  Symbol stock;
  Price price;
  FixedString<4> attribution;
};

struct OrderExecuted {
  MATCHCORE_ITCH_MESSAGE('E', OrderExecuted);
  OrderId order_reference_number{};
  Quantity executed_shares{};
  MatchNumber match_number{};
};

struct OrderExecutedWithPrice {
  MATCHCORE_ITCH_MESSAGE('C', OrderExecutedWithPrice);
  OrderId order_reference_number{};
  Quantity executed_shares{};
  MatchNumber match_number{};
  char printable{};  // 'Y' / 'N'
  Price execution_price;
};

struct OrderCancel {
  MATCHCORE_ITCH_MESSAGE('X', OrderCancel);
  OrderId order_reference_number{};
  Quantity cancelled_shares{};
};

struct OrderDelete {
  MATCHCORE_ITCH_MESSAGE('D', OrderDelete);
  OrderId order_reference_number{};
};

struct OrderReplace {
  MATCHCORE_ITCH_MESSAGE('U', OrderReplace);
  OrderId original_order_reference_number{};
  OrderId new_order_reference_number{};
  Quantity shares{};
  Price price;
};

// ---------------------------------------------------------------------------
// Trade messages
// ---------------------------------------------------------------------------

struct Trade {
  MATCHCORE_ITCH_MESSAGE('P', Trade);
  OrderId order_reference_number{};
  Side side{};
  Quantity shares{};
  Symbol stock;
  Price price;
  MatchNumber match_number{};
};

struct CrossTrade {
  MATCHCORE_ITCH_MESSAGE('Q', CrossTrade);
  std::uint64_t shares{};  // 8 bytes on the wire for crosses
  Symbol stock;
  Price cross_price;
  MatchNumber match_number{};
  char cross_type{};
};

struct BrokenTrade {
  MATCHCORE_ITCH_MESSAGE('B', BrokenTrade);
  MatchNumber match_number{};
};

// ---------------------------------------------------------------------------
// Auction / imbalance messages
// ---------------------------------------------------------------------------

struct Noii {
  MATCHCORE_ITCH_MESSAGE('I', Noii);
  std::uint64_t paired_shares{};
  std::uint64_t imbalance_shares{};
  char imbalance_direction{};
  Symbol stock;
  Price far_price;
  Price near_price;
  Price current_reference_price;
  char cross_type{};
  char price_variation_indicator{};
};

struct RetailPriceImprovement {
  MATCHCORE_ITCH_MESSAGE('N', RetailPriceImprovement);
  Symbol stock;
  char interest_flag{};
};

struct DirectListingCapitalRaise {
  MATCHCORE_ITCH_MESSAGE('O', DirectListingCapitalRaise);
  Symbol stock;
  char open_eligibility_status{};
  Price minimum_allowable_price;
  Price maximum_allowable_price;
  Price near_execution_price;
  std::uint64_t near_execution_time{};
  Price lower_price_range_collar;
  Price upper_price_range_collar;
};

#undef MATCHCORE_ITCH_MESSAGE

}  // namespace matchcore::itch

#endif  // MATCHCORE_ITCH_MESSAGES_HPP
