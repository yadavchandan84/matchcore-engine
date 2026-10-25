#include "matchcore/itch/parser.hpp"

namespace matchcore::itch {

std::string_view to_string(ParseError err) noexcept {
  switch (err) {
    case ParseError::kOk: return "ok";
    case ParseError::kEmpty: return "empty message body";
    case ParseError::kUnknownType: return "unknown message type";
    case ParseError::kBadLength: return "length does not match message type";
    case ParseError::kInvalidField: return "invalid field value";
  }
  return "unrecognized ParseError";
}

}  // namespace matchcore::itch
