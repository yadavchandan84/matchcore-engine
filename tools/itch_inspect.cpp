// itch_inspect: stream a NASDAQ TotalView-ITCH 5.0 file through the parser and
// print per-message-type counts.
//
// Usage:
//   itch_inspect <file.NASDAQ_ITCH50> [max_messages]
//
// The file must be decompressed first (e.g. `gunzip -k 01302020.NASDAQ_ITCH50.gz`).
// The file is read in fixed-size chunks into one buffer allocated at startup;
// incomplete frames at a chunk boundary are carried over to the next read.
// Wall-clock throughput is printed as a sanity check only; the real benchmark
// harness lives in bench/ (Phase 7).

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <span>
#include <string>
#include <vector>

#include "matchcore/itch/parser.hpp"

namespace {

using matchcore::itch::kMaxFrameSize;
using matchcore::itch::parse_stream;

/// Counts every message by type byte. Handling every type forces a full
/// decode of each message, so the timing includes decode cost.
struct CountingHandler {
  std::array<std::uint64_t, 256> by_type{};
  std::uint64_t first_timestamp = 0;
  std::uint64_t last_timestamp = 0;
  std::uint64_t total = 0;

  template <class M>
  void on(const M& msg) noexcept {
    ++by_type[static_cast<unsigned char>(M::kType)];
    if (total == 0) {
      first_timestamp = msg.header.timestamp;
    }
    last_timestamp = msg.header.timestamp;
    ++total;
  }
};

const char* type_name(char t) noexcept {
  switch (t) {
    case 'S': return "System Event";
    case 'R': return "Stock Directory";
    case 'H': return "Stock Trading Action";
    case 'Y': return "Reg SHO Restriction";
    case 'L': return "Market Participant Position";
    case 'V': return "MWCB Decline Level";
    case 'W': return "MWCB Status";
    case 'K': return "IPO Quoting Period Update";
    case 'J': return "LULD Auction Collar";
    case 'h': return "Operational Halt";
    case 'A': return "Add Order";
    case 'F': return "Add Order (MPID)";
    case 'E': return "Order Executed";
    case 'C': return "Order Executed With Price";
    case 'X': return "Order Cancel";
    case 'D': return "Order Delete";
    case 'U': return "Order Replace";
    case 'P': return "Trade (non-cross)";
    case 'Q': return "Cross Trade";
    case 'B': return "Broken Trade";
    case 'I': return "NOII";
    case 'N': return "Retail Price Improvement";
    case 'O': return "Direct Listing Capital Raise";
    default: return "?";
  }
}

std::string format_ns(std::uint64_t ns) {
  const std::uint64_t secs = ns / 1'000'000'000ULL;
  std::array<char, 32> buf{};
  std::snprintf(buf.data(), buf.size(), "%02llu:%02llu:%02llu.%09llu",
                static_cast<unsigned long long>(secs / 3600),
                static_cast<unsigned long long>((secs / 60) % 60),
                static_cast<unsigned long long>(secs % 60),
                static_cast<unsigned long long>(ns % 1'000'000'000ULL));
  return buf.data();
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2 || argc > 3) {
    std::fprintf(stderr, "usage: %s <file.NASDAQ_ITCH50> [max_messages]\n", argv[0]);
    return EXIT_FAILURE;
  }
  // max_messages stops reading once at least that many messages are parsed
  // (checked per chunk). Useful for quick looks at multi-GB files.
  const std::uint64_t max_messages = argc == 3 ? std::strtoull(argv[2], nullptr, 10) : 0;

  std::ifstream in(argv[1], std::ios::binary);
  if (!in) {
    std::fprintf(stderr, "error: cannot open %s\n", argv[1]);
    return EXIT_FAILURE;
  }

  // 4 MiB chunk: comfortably larger than the biggest possible frame, so every
  // read makes forward progress.
  constexpr std::size_t kChunk = std::size_t{4} << 20U;
  static_assert(kChunk >= kMaxFrameSize);
  std::vector<std::byte> buffer(kChunk);

  CountingHandler handler;
  std::uint64_t unknown = 0;
  std::uint64_t malformed = 0;
  std::uint64_t bytes_read = 0;
  std::size_t filled = 0;  // valid bytes at the front of `buffer`

  const auto start = std::chrono::steady_clock::now();
  while (true) {
    in.read(reinterpret_cast<char*>(buffer.data() + filled),  // NOLINT(*-reinterpret-cast)
            static_cast<std::streamsize>(buffer.size() - filled));
    const auto got = static_cast<std::size_t>(in.gcount());
    bytes_read += got;
    filled += got;
    if (got == 0) {
      break;
    }

    const auto r = parse_stream(std::span<const std::byte>{buffer.data(), filled}, handler);
    unknown += r.unknown_types;
    malformed += r.malformed;

    // Carry the incomplete trailing frame to the front of the buffer.
    const std::size_t tail = filled - r.bytes_consumed;
    std::memmove(buffer.data(), buffer.data() + r.bytes_consumed, tail);
    filled = tail;

    if (max_messages != 0 && handler.total >= max_messages) {
      break;
    }
  }
  const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start);

  std::printf("%-30s %4s %15s\n", "message type", "code", "count");
  for (std::size_t t = 0; t < handler.by_type.size(); ++t) {
    if (handler.by_type[t] != 0) {
      const char code = static_cast<char>(t);
      std::printf("%-30s %4c %15llu\n", type_name(code), code,
                  static_cast<unsigned long long>(handler.by_type[t]));
    }
  }
  std::printf("\n");
  std::printf("messages decoded : %llu\n", static_cast<unsigned long long>(handler.total));
  std::printf("unknown types    : %llu\n", static_cast<unsigned long long>(unknown));
  std::printf("malformed frames : %llu\n", static_cast<unsigned long long>(malformed));
  std::printf("trailing bytes   : %zu%s\n", filled, filled != 0 ? "  (incomplete final frame)" : "");
  std::printf("bytes read       : %llu\n", static_cast<unsigned long long>(bytes_read));
  if (handler.total != 0) {
    std::printf("time range       : %s .. %s\n", format_ns(handler.first_timestamp).c_str(),
                format_ns(handler.last_timestamp).c_str());
  }
  if (elapsed.count() > 0.0) {
    const double secs = elapsed.count();
    std::printf("wall time        : %.3f s  (%.1f M msgs/s, %.1f MB/s incl. file I/O)\n", secs,
                static_cast<double>(handler.total) / secs / 1e6,
                static_cast<double>(bytes_read) / secs / 1e6);
  }

  // Non-zero exit if the file was not clean, so scripts can detect corruption.
  return (unknown == 0 && malformed == 0 && filled == 0) ? EXIT_SUCCESS : 2;
}
