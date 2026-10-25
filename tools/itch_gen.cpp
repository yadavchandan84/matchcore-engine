// itch_gen: write a deterministic synthetic ITCH 5.0 session to a file.
//
// Usage:
//   itch_gen <out.NASDAQ_ITCH50> [messages] [symbols] [seed]
//
// The output has the same framing as a real NASDAQ file, so it can be fed to
// itch_inspect and replay_bench, and used as a small fixture when the real
// multi-gigabyte sample is not available (e.g. in CI).

#include <cstdio>
#include <cstdlib>
#include <fstream>

#include "matchcore/itch/synthetic.hpp"

int main(int argc, char** argv) {
  if (argc < 2 || argc > 5) {
    std::fprintf(stderr, "usage: %s <out.NASDAQ_ITCH50> [messages] [symbols] [seed]\n", argv[0]);
    return EXIT_FAILURE;
  }
  matchcore::itch::SyntheticConfig cfg;
  if (argc > 2) {
    cfg.messages = std::strtoull(argv[2], nullptr, 10);
  }
  if (argc > 3) {
    cfg.symbols = static_cast<std::uint16_t>(std::strtoul(argv[3], nullptr, 10));
  }
  if (argc > 4) {
    cfg.seed = std::strtoull(argv[4], nullptr, 10);
  }
  if (cfg.symbols == 0) {
    std::fprintf(stderr, "symbols must be >= 1\n");
    return EXIT_FAILURE;
  }

  const auto data = matchcore::itch::generate_session(cfg);
  std::ofstream out(argv[1], std::ios::binary);
  out.write(reinterpret_cast<const char*>(data.data()),  // NOLINT(*-reinterpret-cast)
            static_cast<std::streamsize>(data.size()));
  if (!out) {
    std::fprintf(stderr, "error: failed to write %s\n", argv[1]);
    return EXIT_FAILURE;
  }
  std::printf("wrote %zu bytes (%zu order messages, %u symbols) to %s\n", data.size(),
              cfg.messages, static_cast<unsigned>(cfg.symbols), argv[1]);
  return EXIT_SUCCESS;
}
