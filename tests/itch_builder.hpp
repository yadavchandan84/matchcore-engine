#ifndef MATCHCORE_TESTS_ITCH_BUILDER_HPP
#define MATCHCORE_TESTS_ITCH_BUILDER_HPP

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

// Test-only helper that encodes ITCH 5.0 messages byte by byte, independently
// of the wire structs under test. Writing fields explicitly in spec order is
// what makes the decode tests meaningful: a wrong offset or size in wire.hpp
// shows up as a mismatched field.
namespace matchcore::itch::test {

class MessageBuilder {
 public:
  MessageBuilder& ch(char c) {
    bytes_.push_back(static_cast<std::byte>(c));
    return *this;
  }

  /// Big-endian unsigned integer of `width` bytes.
  MessageBuilder& be(std::uint64_t value, std::size_t width) {
    for (std::size_t i = width; i > 0; --i) {
      bytes_.push_back(static_cast<std::byte>((value >> (8U * (i - 1))) & 0xFFU));
    }
    return *this;
  }

  MessageBuilder& u16(std::uint64_t v) { return be(v, 2); }
  MessageBuilder& u32(std::uint64_t v) { return be(v, 4); }
  MessageBuilder& u48(std::uint64_t v) { return be(v, 6); }
  MessageBuilder& u64(std::uint64_t v) { return be(v, 8); }

  /// Alpha field: left-justified, right-padded with spaces to `width`.
  MessageBuilder& alpha(std::string_view s, std::size_t width) {
    for (std::size_t i = 0; i < width; ++i) {
      ch(i < s.size() ? s[i] : ' ');
    }
    return *this;
  }

  /// Common 11-byte header.
  MessageBuilder& header(char type, std::uint16_t locate, std::uint16_t tracking,
                         std::uint64_t timestamp) {
    return ch(type).u16(locate).u16(tracking).u48(timestamp);
  }

  [[nodiscard]] std::span<const std::byte> body() const { return bytes_; }
  [[nodiscard]] std::size_t size() const { return bytes_.size(); }

  /// Body with its 2-byte big-endian length prefix, as it appears in a file.
  [[nodiscard]] std::vector<std::byte> framed() const {
    std::vector<std::byte> out;
    out.reserve(bytes_.size() + 2);
    out.push_back(static_cast<std::byte>((bytes_.size() >> 8U) & 0xFFU));
    out.push_back(static_cast<std::byte>(bytes_.size() & 0xFFU));
    out.insert(out.end(), bytes_.begin(), bytes_.end());
    return out;
  }

 private:
  std::vector<std::byte> bytes_;
};

/// Append a framed message to a stream buffer.
inline void append(std::vector<std::byte>& stream, const MessageBuilder& msg) {
  const auto f = msg.framed();
  stream.insert(stream.end(), f.begin(), f.end());
}

}  // namespace matchcore::itch::test

#endif  // MATCHCORE_TESTS_ITCH_BUILDER_HPP
