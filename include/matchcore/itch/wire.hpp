#ifndef MATCHCORE_ITCH_WIRE_HPP
#define MATCHCORE_ITCH_WIRE_HPP

#include <cstddef>
#include <cstdint>
#include <type_traits>

// Wire-level layout of NASDAQ TotalView-ITCH 5.0 messages.
//
// Every field is built from byte arrays, so every struct has alignment 1 and
// therefore no padding: `sizeof` equals the exact on-the-wire size without any
// compiler-specific `#pragma pack`. These structs are the single source of
// truth for field offsets; the `static_assert`s at the bottom pin each one to
// the size given in the specification.
//
// Multi-byte integers are big-endian on the wire. They are decoded by
// `BeUint<N>::value()`, which assembles the value with shifts; GCC and Clang
// lower that pattern to a single load + `bswap`/`movbe`.
//
// Message bodies below exclude the 2-byte length prefix used for framing.
namespace matchcore::itch::wire {

namespace detail {
// Smallest unsigned type that can hold an N-byte wire integer.
template <std::size_t N>
using UintFor = std::conditional_t<
    (N <= 1), std::uint8_t,
    std::conditional_t<(N <= 2), std::uint16_t,
                       std::conditional_t<(N <= 4), std::uint32_t, std::uint64_t>>>;
}  // namespace detail

/// N-byte big-endian unsigned integer.
template <std::size_t N>
struct BeUint {
  static_assert(N >= 1 && N <= 8, "ITCH integers are 1..8 bytes");
  using value_type = detail::UintFor<N>;

  unsigned char bytes[N];  // NOLINT(*-avoid-c-arrays): exact wire layout

  [[nodiscard]] constexpr value_type value() const noexcept {
    value_type v = 0;
    for (std::size_t i = 0; i < N; ++i) {
      v = static_cast<value_type>((v << 8U) | static_cast<value_type>(bytes[i]));
    }
    return v;
  }
};

/// N-byte ASCII field, space-padded on the right.
template <std::size_t N>
struct Alpha {
  char chars[N];  // NOLINT(*-avoid-c-arrays): exact wire layout
};

/// Single ASCII character field.
using Char = char;

/// Common 11-byte header present on every ITCH 5.0 message.
struct Header {
  Char message_type;
  BeUint<2> stock_locate;
  BeUint<2> tracking_number;
  BeUint<6> timestamp;  // nanoseconds since midnight
};

// ---------------------------------------------------------------------------
// System / administrative messages
// ---------------------------------------------------------------------------

struct SystemEvent {  // 'S'
  Header header;
  Char event_code;
};

struct StockDirectory {  // 'R'
  Header header;
  Alpha<8> stock;
  Char market_category;
  Char financial_status_indicator;
  BeUint<4> round_lot_size;
  Char round_lots_only;
  Char issue_classification;
  Alpha<2> issue_sub_type;
  Char authenticity;
  Char short_sale_threshold_indicator;
  Char ipo_flag;
  Char luld_reference_price_tier;
  Char etp_flag;
  BeUint<4> etp_leverage_factor;
  Char inverse_indicator;
};

struct StockTradingAction {  // 'H'
  Header header;
  Alpha<8> stock;
  Char trading_state;
  Char reserved;
  Alpha<4> reason;
};

struct RegShoRestriction {  // 'Y'
  Header header;
  Alpha<8> stock;
  Char reg_sho_action;
};

struct MarketParticipantPosition {  // 'L'
  Header header;
  Alpha<4> mpid;
  Alpha<8> stock;
  Char primary_market_maker;
  Char market_maker_mode;
  Char market_participant_state;
};

struct MwcbDeclineLevel {  // 'V'
  Header header;
  BeUint<8> level1;  // Price (8)
  BeUint<8> level2;
  BeUint<8> level3;
};

struct MwcbStatus {  // 'W'
  Header header;
  Char breached_level;
};

struct IpoQuotingPeriodUpdate {  // 'K'
  Header header;
  Alpha<8> stock;
  BeUint<4> ipo_quotation_release_time;  // seconds since midnight
  Char ipo_quotation_release_qualifier;
  BeUint<4> ipo_price;
};

struct LuldAuctionCollar {  // 'J'
  Header header;
  Alpha<8> stock;
  BeUint<4> auction_collar_reference_price;
  BeUint<4> upper_auction_collar_price;
  BeUint<4> lower_auction_collar_price;
  BeUint<4> auction_collar_extension;
};

struct OperationalHalt {  // 'h'
  Header header;
  Alpha<8> stock;
  Char market_code;
  Char operational_halt_action;
};

// ---------------------------------------------------------------------------
// Order book messages
// ---------------------------------------------------------------------------

struct AddOrder {  // 'A'
  Header header;
  BeUint<8> order_reference_number;
  Char buy_sell_indicator;
  BeUint<4> shares;
  Alpha<8> stock;
  BeUint<4> price;
};

struct AddOrderMpid {  // 'F'
  Header header;
  BeUint<8> order_reference_number;
  Char buy_sell_indicator;
  BeUint<4> shares;
  Alpha<8> stock;
  BeUint<4> price;
  Alpha<4> attribution;
};

struct OrderExecuted {  // 'E'
  Header header;
  BeUint<8> order_reference_number;
  BeUint<4> executed_shares;
  BeUint<8> match_number;
};

struct OrderExecutedWithPrice {  // 'C'
  Header header;
  BeUint<8> order_reference_number;
  BeUint<4> executed_shares;
  BeUint<8> match_number;
  Char printable;
  BeUint<4> execution_price;
};

struct OrderCancel {  // 'X'
  Header header;
  BeUint<8> order_reference_number;
  BeUint<4> cancelled_shares;
};

struct OrderDelete {  // 'D'
  Header header;
  BeUint<8> order_reference_number;
};

struct OrderReplace {  // 'U'
  Header header;
  BeUint<8> original_order_reference_number;
  BeUint<8> new_order_reference_number;
  BeUint<4> shares;
  BeUint<4> price;
};

// ---------------------------------------------------------------------------
// Trade messages
// ---------------------------------------------------------------------------

struct Trade {  // 'P' (non-cross)
  Header header;
  BeUint<8> order_reference_number;
  Char buy_sell_indicator;
  BeUint<4> shares;
  Alpha<8> stock;
  BeUint<4> price;
  BeUint<8> match_number;
};

struct CrossTrade {  // 'Q'
  Header header;
  BeUint<8> shares;
  Alpha<8> stock;
  BeUint<4> cross_price;
  BeUint<8> match_number;
  Char cross_type;
};

struct BrokenTrade {  // 'B'
  Header header;
  BeUint<8> match_number;
};

// ---------------------------------------------------------------------------
// Auction / imbalance messages
// ---------------------------------------------------------------------------

struct Noii {  // 'I' Net Order Imbalance Indicator
  Header header;
  BeUint<8> paired_shares;
  BeUint<8> imbalance_shares;
  Char imbalance_direction;
  Alpha<8> stock;
  BeUint<4> far_price;
  BeUint<4> near_price;
  BeUint<4> current_reference_price;
  Char cross_type;
  Char price_variation_indicator;
};

struct RetailPriceImprovement {  // 'N'
  Header header;
  Alpha<8> stock;
  Char interest_flag;
};

struct DirectListingCapitalRaise {  // 'O'
  Header header;
  Alpha<8> stock;
  Char open_eligibility_status;
  BeUint<4> minimum_allowable_price;
  BeUint<4> maximum_allowable_price;
  BeUint<4> near_execution_price;
  BeUint<8> near_execution_time;
  BeUint<4> lower_price_range_collar;
  BeUint<4> upper_price_range_collar;
};

// ---------------------------------------------------------------------------
// Layout guarantees. Sizes are taken from the ITCH 5.0 specification.
// ---------------------------------------------------------------------------

template <class T>
inline constexpr bool kIsWireLayout =
    std::is_trivially_copyable_v<T> && std::is_standard_layout_v<T> && alignof(T) == 1;

#define MATCHCORE_WIRE_SIZE(T, N)                                  \
  static_assert(sizeof(T) == (N), #T " has wrong wire size");      \
  static_assert(kIsWireLayout<T>, #T " must be a packed POD layout")

MATCHCORE_WIRE_SIZE(Header, 11);
MATCHCORE_WIRE_SIZE(SystemEvent, 12);
MATCHCORE_WIRE_SIZE(StockDirectory, 39);
MATCHCORE_WIRE_SIZE(StockTradingAction, 25);
MATCHCORE_WIRE_SIZE(RegShoRestriction, 20);
MATCHCORE_WIRE_SIZE(MarketParticipantPosition, 26);
MATCHCORE_WIRE_SIZE(MwcbDeclineLevel, 35);
MATCHCORE_WIRE_SIZE(MwcbStatus, 12);
MATCHCORE_WIRE_SIZE(IpoQuotingPeriodUpdate, 28);
MATCHCORE_WIRE_SIZE(LuldAuctionCollar, 35);
MATCHCORE_WIRE_SIZE(OperationalHalt, 21);
MATCHCORE_WIRE_SIZE(AddOrder, 36);
MATCHCORE_WIRE_SIZE(AddOrderMpid, 40);
MATCHCORE_WIRE_SIZE(OrderExecuted, 31);
MATCHCORE_WIRE_SIZE(OrderExecutedWithPrice, 36);
MATCHCORE_WIRE_SIZE(OrderCancel, 23);
MATCHCORE_WIRE_SIZE(OrderDelete, 19);
MATCHCORE_WIRE_SIZE(OrderReplace, 35);
MATCHCORE_WIRE_SIZE(Trade, 44);
MATCHCORE_WIRE_SIZE(CrossTrade, 40);
MATCHCORE_WIRE_SIZE(BrokenTrade, 19);
MATCHCORE_WIRE_SIZE(Noii, 50);
MATCHCORE_WIRE_SIZE(RetailPriceImprovement, 20);
MATCHCORE_WIRE_SIZE(DirectListingCapitalRaise, 48);

#undef MATCHCORE_WIRE_SIZE

}  // namespace matchcore::itch::wire

#endif  // MATCHCORE_ITCH_WIRE_HPP
