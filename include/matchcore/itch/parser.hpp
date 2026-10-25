#ifndef MATCHCORE_ITCH_PARSER_HPP
#define MATCHCORE_ITCH_PARSER_HPP

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>

#include "matchcore/itch/messages.hpp"
#include "matchcore/itch/wire.hpp"
#include "matchcore/types.hpp"

// NASDAQ TotalView-ITCH 5.0 parser.
//
// Layers (each usable on its own):
//   1. decode()        : wire layout  -> host-endian message struct
//   2. parse_message() : one message body -> type dispatch -> handler.on(msg)
//   3. parse_stream()  : length-prefixed byte stream -> parse_message() per frame
//
// Properties:
//   - No heap allocation and no exceptions anywhere in this header.
//   - Static dispatch: the handler is a template parameter, so every
//     handler.on(...) call is resolved at compile time and can be inlined.
//     There are no virtual calls on the hot path.
//   - Handlers only implement the overloads they care about. For a message
//     type with no matching `on` overload, `if constexpr` removes the decode
//     entirely; the frame is only length-checked.
//   - Input is read in place. Each body is copied with std::memcpy into an
//     alignment-1 wire struct, which is the well-defined way to reinterpret
//     bytes in C++20 (a reinterpret_cast to a struct pointer is not). The
//     optimizer turns that copy into direct loads of the fields actually used.
namespace matchcore::itch {

/// Outcome of parsing a single message body.
enum class ParseError : std::uint8_t {
  kOk,            ///< Decoded and delivered (or intentionally ignored).
  kEmpty,         ///< Zero-length body: no message type byte.
  kUnknownType,   ///< Message type byte is not part of ITCH 5.0.
  kBadLength,     ///< Body length does not match the type's fixed size.
  kInvalidField,  ///< A field that is decoded strictly holds an invalid value.
};

/// Human-readable name for a ParseError (static storage, never allocates).
[[nodiscard]] std::string_view to_string(ParseError err) noexcept;

/// Size of the big-endian length prefix in front of every message.
inline constexpr std::size_t kLengthPrefixSize = 2;

/// Largest frame the 2-byte prefix can describe. Chunked readers must keep at
/// least this many bytes of buffer to guarantee forward progress.
inline constexpr std::size_t kMaxFrameSize = kLengthPrefixSize + 0xFFFF;

/// Aggregate result of parsing a byte stream.
struct StreamResult {
  /// Bytes consumed from the front of the input. Always ends on a frame
  /// boundary; anything after it is an incomplete trailing frame that the
  /// caller should carry over to the next chunk.
  std::size_t bytes_consumed{0};
  std::uint64_t messages{0};       ///< Frames successfully decoded.
  std::uint64_t unknown_types{0};  ///< Frames skipped: unknown message type.
  std::uint64_t malformed{0};      ///< Frames skipped: empty / bad length / bad field.
};

// ---------------------------------------------------------------------------
// Layer 1: decoding
// ---------------------------------------------------------------------------
namespace detail {

/// Copy `sizeof(W)` bytes into a wire struct. Caller guarantees the length.
template <class W>
[[nodiscard]] inline W load_wire(const std::byte* src) noexcept {
  static_assert(wire::kIsWireLayout<W>);
  W w;  // NOLINT(cppcoreguidelines-pro-type-member-init): filled by memcpy
  std::memcpy(&w, src, sizeof(W));
  return w;
}

[[nodiscard]] constexpr MessageHeader decode_header(const wire::Header& h) noexcept {
  return {h.stock_locate.value(), h.tracking_number.value(), h.timestamp.value()};
}

template <std::size_t N>
[[nodiscard]] constexpr FixedString<N> decode_alpha(const wire::Alpha<N>& a) noexcept {
  FixedString<N> s;
  for (std::size_t i = 0; i < N; ++i) {
    s.chars[i] = a.chars[i];
  }
  return s;
}

[[nodiscard]] constexpr Price decode_price(const wire::BeUint<4>& p) noexcept {
  return Price{p.value()};
}

/// 'B' -> kBuy, 'S' -> kSell. Anything else is rejected.
[[nodiscard]] constexpr bool decode_side(char c, Side& out) noexcept {
  if (c == 'B') {
    out = Side::kBuy;
    return true;
  }
  if (c == 'S') {
    out = Side::kSell;
    return true;
  }
  return false;
}

}  // namespace detail

// One decode() overload per message type. Each returns false only when a
// strictly-validated field (currently: Buy/Sell indicator) is invalid.

constexpr bool decode(const wire::SystemEvent& w, SystemEvent& m) noexcept {
  m.header = detail::decode_header(w.header);
  m.event_code = w.event_code;
  return true;
}

constexpr bool decode(const wire::StockDirectory& w, StockDirectory& m) noexcept {
  m.header = detail::decode_header(w.header);
  m.stock = detail::decode_alpha(w.stock);
  m.market_category = w.market_category;
  m.financial_status_indicator = w.financial_status_indicator;
  m.round_lot_size = w.round_lot_size.value();
  m.round_lots_only = w.round_lots_only;
  m.issue_classification = w.issue_classification;
  m.issue_sub_type = detail::decode_alpha(w.issue_sub_type);
  m.authenticity = w.authenticity;
  m.short_sale_threshold_indicator = w.short_sale_threshold_indicator;
  m.ipo_flag = w.ipo_flag;
  m.luld_reference_price_tier = w.luld_reference_price_tier;
  m.etp_flag = w.etp_flag;
  m.etp_leverage_factor = w.etp_leverage_factor.value();
  m.inverse_indicator = w.inverse_indicator;
  return true;
}

constexpr bool decode(const wire::StockTradingAction& w, StockTradingAction& m) noexcept {
  m.header = detail::decode_header(w.header);
  m.stock = detail::decode_alpha(w.stock);
  m.trading_state = w.trading_state;
  m.reserved = w.reserved;
  m.reason = detail::decode_alpha(w.reason);
  return true;
}

constexpr bool decode(const wire::RegShoRestriction& w, RegShoRestriction& m) noexcept {
  m.header = detail::decode_header(w.header);
  m.stock = detail::decode_alpha(w.stock);
  m.reg_sho_action = w.reg_sho_action;
  return true;
}

constexpr bool decode(const wire::MarketParticipantPosition& w,
                      MarketParticipantPosition& m) noexcept {
  m.header = detail::decode_header(w.header);
  m.mpid = detail::decode_alpha(w.mpid);
  m.stock = detail::decode_alpha(w.stock);
  m.primary_market_maker = w.primary_market_maker;
  m.market_maker_mode = w.market_maker_mode;
  m.market_participant_state = w.market_participant_state;
  return true;
}

constexpr bool decode(const wire::MwcbDeclineLevel& w, MwcbDeclineLevel& m) noexcept {
  m.header = detail::decode_header(w.header);
  m.level1 = w.level1.value();
  m.level2 = w.level2.value();
  m.level3 = w.level3.value();
  return true;
}

constexpr bool decode(const wire::MwcbStatus& w, MwcbStatus& m) noexcept {
  m.header = detail::decode_header(w.header);
  m.breached_level = w.breached_level;
  return true;
}

constexpr bool decode(const wire::IpoQuotingPeriodUpdate& w, IpoQuotingPeriodUpdate& m) noexcept {
  m.header = detail::decode_header(w.header);
  m.stock = detail::decode_alpha(w.stock);
  m.ipo_quotation_release_time = w.ipo_quotation_release_time.value();
  m.ipo_quotation_release_qualifier = w.ipo_quotation_release_qualifier;
  m.ipo_price = detail::decode_price(w.ipo_price);
  return true;
}

constexpr bool decode(const wire::LuldAuctionCollar& w, LuldAuctionCollar& m) noexcept {
  m.header = detail::decode_header(w.header);
  m.stock = detail::decode_alpha(w.stock);
  m.auction_collar_reference_price = detail::decode_price(w.auction_collar_reference_price);
  m.upper_auction_collar_price = detail::decode_price(w.upper_auction_collar_price);
  m.lower_auction_collar_price = detail::decode_price(w.lower_auction_collar_price);
  m.auction_collar_extension = w.auction_collar_extension.value();
  return true;
}

constexpr bool decode(const wire::OperationalHalt& w, OperationalHalt& m) noexcept {
  m.header = detail::decode_header(w.header);
  m.stock = detail::decode_alpha(w.stock);
  m.market_code = w.market_code;
  m.operational_halt_action = w.operational_halt_action;
  return true;
}

constexpr bool decode(const wire::AddOrder& w, AddOrder& m) noexcept {
  m.header = detail::decode_header(w.header);
  m.order_reference_number = w.order_reference_number.value();
  m.shares = w.shares.value();
  m.stock = detail::decode_alpha(w.stock);
  m.price = detail::decode_price(w.price);
  return detail::decode_side(w.buy_sell_indicator, m.side);
}

constexpr bool decode(const wire::AddOrderMpid& w, AddOrderMpid& m) noexcept {
  m.header = detail::decode_header(w.header);
  m.order_reference_number = w.order_reference_number.value();
  m.shares = w.shares.value();
  m.stock = detail::decode_alpha(w.stock);
  m.price = detail::decode_price(w.price);
  m.attribution = detail::decode_alpha(w.attribution);
  return detail::decode_side(w.buy_sell_indicator, m.side);
}

constexpr bool decode(const wire::OrderExecuted& w, OrderExecuted& m) noexcept {
  m.header = detail::decode_header(w.header);
  m.order_reference_number = w.order_reference_number.value();
  m.executed_shares = w.executed_shares.value();
  m.match_number = w.match_number.value();
  return true;
}

constexpr bool decode(const wire::OrderExecutedWithPrice& w, OrderExecutedWithPrice& m) noexcept {
  m.header = detail::decode_header(w.header);
  m.order_reference_number = w.order_reference_number.value();
  m.executed_shares = w.executed_shares.value();
  m.match_number = w.match_number.value();
  m.printable = w.printable;
  m.execution_price = detail::decode_price(w.execution_price);
  return true;
}

constexpr bool decode(const wire::OrderCancel& w, OrderCancel& m) noexcept {
  m.header = detail::decode_header(w.header);
  m.order_reference_number = w.order_reference_number.value();
  m.cancelled_shares = w.cancelled_shares.value();
  return true;
}

constexpr bool decode(const wire::OrderDelete& w, OrderDelete& m) noexcept {
  m.header = detail::decode_header(w.header);
  m.order_reference_number = w.order_reference_number.value();
  return true;
}

constexpr bool decode(const wire::OrderReplace& w, OrderReplace& m) noexcept {
  m.header = detail::decode_header(w.header);
  m.original_order_reference_number = w.original_order_reference_number.value();
  m.new_order_reference_number = w.new_order_reference_number.value();
  m.shares = w.shares.value();
  m.price = detail::decode_price(w.price);
  return true;
}

constexpr bool decode(const wire::Trade& w, Trade& m) noexcept {
  m.header = detail::decode_header(w.header);
  m.order_reference_number = w.order_reference_number.value();
  m.shares = w.shares.value();
  m.stock = detail::decode_alpha(w.stock);
  m.price = detail::decode_price(w.price);
  m.match_number = w.match_number.value();
  return detail::decode_side(w.buy_sell_indicator, m.side);
}

constexpr bool decode(const wire::CrossTrade& w, CrossTrade& m) noexcept {
  m.header = detail::decode_header(w.header);
  m.shares = w.shares.value();
  m.stock = detail::decode_alpha(w.stock);
  m.cross_price = detail::decode_price(w.cross_price);
  m.match_number = w.match_number.value();
  m.cross_type = w.cross_type;
  return true;
}

constexpr bool decode(const wire::BrokenTrade& w, BrokenTrade& m) noexcept {
  m.header = detail::decode_header(w.header);
  m.match_number = w.match_number.value();
  return true;
}

constexpr bool decode(const wire::Noii& w, Noii& m) noexcept {
  m.header = detail::decode_header(w.header);
  m.paired_shares = w.paired_shares.value();
  m.imbalance_shares = w.imbalance_shares.value();
  m.imbalance_direction = w.imbalance_direction;
  m.stock = detail::decode_alpha(w.stock);
  m.far_price = detail::decode_price(w.far_price);
  m.near_price = detail::decode_price(w.near_price);
  m.current_reference_price = detail::decode_price(w.current_reference_price);
  m.cross_type = w.cross_type;
  m.price_variation_indicator = w.price_variation_indicator;
  return true;
}

constexpr bool decode(const wire::RetailPriceImprovement& w, RetailPriceImprovement& m) noexcept {
  m.header = detail::decode_header(w.header);
  m.stock = detail::decode_alpha(w.stock);
  m.interest_flag = w.interest_flag;
  return true;
}

constexpr bool decode(const wire::DirectListingCapitalRaise& w,
                      DirectListingCapitalRaise& m) noexcept {
  m.header = detail::decode_header(w.header);
  m.stock = detail::decode_alpha(w.stock);
  m.open_eligibility_status = w.open_eligibility_status;
  m.minimum_allowable_price = detail::decode_price(w.minimum_allowable_price);
  m.maximum_allowable_price = detail::decode_price(w.maximum_allowable_price);
  m.near_execution_price = detail::decode_price(w.near_execution_price);
  m.near_execution_time = w.near_execution_time.value();
  m.lower_price_range_collar = detail::decode_price(w.lower_price_range_collar);
  m.upper_price_range_collar = detail::decode_price(w.upper_price_range_collar);
  return true;
}

// ---------------------------------------------------------------------------
// Layer 2: single-message dispatch
// ---------------------------------------------------------------------------

/// True if `Handler` has an `on(const M&)` overload.
template <class Handler, class M>
concept HandlesMessage = requires(Handler& h, const M& m) { h.on(m); };

namespace detail {

template <class M, class Handler>
[[nodiscard]] inline ParseError dispatch(std::span<const std::byte> body, Handler& handler) {
  if (body.size() != M::kWireSize) {
    return ParseError::kBadLength;
  }
  if constexpr (HandlesMessage<Handler, M>) {
    M msg;
    if (!decode(load_wire<typename M::Wire>(body.data()), msg)) {
      return ParseError::kInvalidField;
    }
    handler.on(static_cast<const M&>(msg));
  }
  return ParseError::kOk;
}

}  // namespace detail

/// Parse one ITCH message body (without the 2-byte length prefix) and deliver
/// it to `handler.on(const Msg&)` if the handler has such an overload.
///
/// Message types the handler does not handle are length-checked but not
/// decoded. Exceptions thrown by the handler propagate unchanged.
template <class Handler>
[[nodiscard]] ParseError parse_message(std::span<const std::byte> body, Handler& handler) {
  if (body.empty()) {
    return ParseError::kEmpty;
  }
  switch (static_cast<char>(body[0])) {
    // Ordered roughly by frequency in a real session: order-flow messages
    // dominate, administrative messages are rare.
    case AddOrder::kType: return detail::dispatch<AddOrder>(body, handler);
    case OrderDelete::kType: return detail::dispatch<OrderDelete>(body, handler);
    case OrderReplace::kType: return detail::dispatch<OrderReplace>(body, handler);
    case OrderExecuted::kType: return detail::dispatch<OrderExecuted>(body, handler);
    case OrderCancel::kType: return detail::dispatch<OrderCancel>(body, handler);
    case AddOrderMpid::kType: return detail::dispatch<AddOrderMpid>(body, handler);
    case Trade::kType: return detail::dispatch<Trade>(body, handler);
    case OrderExecutedWithPrice::kType:
      return detail::dispatch<OrderExecutedWithPrice>(body, handler);
    case Noii::kType: return detail::dispatch<Noii>(body, handler);
    case CrossTrade::kType: return detail::dispatch<CrossTrade>(body, handler);
    case BrokenTrade::kType: return detail::dispatch<BrokenTrade>(body, handler);
    case SystemEvent::kType: return detail::dispatch<SystemEvent>(body, handler);
    case StockDirectory::kType: return detail::dispatch<StockDirectory>(body, handler);
    case StockTradingAction::kType: return detail::dispatch<StockTradingAction>(body, handler);
    case RegShoRestriction::kType: return detail::dispatch<RegShoRestriction>(body, handler);
    case MarketParticipantPosition::kType:
      return detail::dispatch<MarketParticipantPosition>(body, handler);
    case MwcbDeclineLevel::kType: return detail::dispatch<MwcbDeclineLevel>(body, handler);
    case MwcbStatus::kType: return detail::dispatch<MwcbStatus>(body, handler);
    case IpoQuotingPeriodUpdate::kType:
      return detail::dispatch<IpoQuotingPeriodUpdate>(body, handler);
    case LuldAuctionCollar::kType: return detail::dispatch<LuldAuctionCollar>(body, handler);
    case OperationalHalt::kType: return detail::dispatch<OperationalHalt>(body, handler);
    case RetailPriceImprovement::kType:
      return detail::dispatch<RetailPriceImprovement>(body, handler);
    case DirectListingCapitalRaise::kType:
      return detail::dispatch<DirectListingCapitalRaise>(body, handler);
    default: return ParseError::kUnknownType;
  }
}

// ---------------------------------------------------------------------------
// Layer 3: stream framing
// ---------------------------------------------------------------------------

/// Parse a buffer of length-prefixed ITCH messages (the layout of a
/// `*.NASDAQ_ITCH50` file: [len:u16 BE][body:len bytes] repeated).
///
/// Stops at the first incomplete frame and reports how many bytes were
/// consumed, so a chunked reader can move the tail to the front of its buffer
/// and append more data. A malformed or unknown frame is counted and skipped;
/// the framing itself stays valid, so parsing continues with the next frame.
template <class Handler>
[[nodiscard]] StreamResult parse_stream(std::span<const std::byte> buffer, Handler& handler) {
  StreamResult result;
  std::size_t pos = 0;
  const std::size_t size = buffer.size();

  while (size - pos >= kLengthPrefixSize) {
    const auto len_wire = detail::load_wire<wire::BeUint<2>>(buffer.data() + pos);
    const std::size_t len = len_wire.value();
    if (size - pos - kLengthPrefixSize < len) {
      break;  // incomplete trailing frame
    }
    const auto body = buffer.subspan(pos + kLengthPrefixSize, len);
    switch (parse_message(body, handler)) {
      case ParseError::kOk: ++result.messages; break;
      case ParseError::kUnknownType: ++result.unknown_types; break;
      case ParseError::kEmpty:
      case ParseError::kBadLength:
      case ParseError::kInvalidField: ++result.malformed; break;
    }
    pos += kLengthPrefixSize + len;
  }

  result.bytes_consumed = pos;
  return result;
}

}  // namespace matchcore::itch

#endif  // MATCHCORE_ITCH_PARSER_HPP
