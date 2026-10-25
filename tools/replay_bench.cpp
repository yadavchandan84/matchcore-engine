// replay_bench: end-to-end replay benchmark (ITCH parse + book rebuild).
//
// Usage:
//   replay_bench [--file <path.NASDAQ_ITCH50> | --synthetic <messages>]
//                [--runs N] [--book fast|ref] [--max-orders N]
//
// For each run it performs two passes over the input with a fresh book:
//   1. throughput pass : no per-message instrumentation -> msgs/s
//   2. latency pass    : steady_clock around every parse_message() call
//                        (decode + book update) -> p50 / p99 / p99.9 / max
// A warm-up pass runs first and is discarded. Timer overhead (two
// back-to-back clock reads) is measured and reported, not subtracted.
// The latency histogram is fixed-size (1 ns buckets up to 64 us, plus an
// overflow bucket) so the measurement itself never allocates per message.

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "matchcore/book/itch_book_builder.hpp"
#include "matchcore/book/order_book.hpp"
#include "matchcore/book/reference_book.hpp"
#include "matchcore/itch/parser.hpp"
#include "matchcore/itch/synthetic.hpp"

namespace {

using namespace matchcore;
using Clock = std::chrono::steady_clock;

struct Options {
  std::string file;
  std::size_t synthetic = 0;
  int runs = 5;
  bool reference = false;
  std::size_t max_orders = std::size_t{1} << 22U;  // ~4M resting orders
};

/// Fixed-size latency histogram in nanoseconds.
class Histogram {
 public:
  static constexpr std::size_t kBuckets = 65'536;
  Histogram() : counts_(kBuckets + 1, 0) {}

  void add(std::uint64_t ns) noexcept {
    ++counts_[std::min<std::uint64_t>(ns, kBuckets)];
    max_ = std::max(max_, ns);
    ++total_;
  }
  [[nodiscard]] std::uint64_t percentile(double p) const noexcept {
    const auto target = static_cast<std::uint64_t>(p * static_cast<double>(total_));
    std::uint64_t seen = 0;
    for (std::size_t i = 0; i < counts_.size(); ++i) {
      seen += counts_[i];
      if (seen > target) {
        return i;
      }
    }
    return max_;
  }
  [[nodiscard]] std::uint64_t max() const noexcept { return max_; }

 private:
  std::vector<std::uint64_t> counts_;
  std::uint64_t max_{0};
  std::uint64_t total_{0};
};

/// Feeds the input to `fn(span)` in chunks that always end on a frame
/// boundary. In-memory input is a single chunk.
class Source {
 public:
  explicit Source(const Options& opt) {
    if (opt.file.empty()) {
      itch::SyntheticConfig cfg;
      cfg.messages = opt.synthetic;
      cfg.symbols = 100;
      memory_ = itch::generate_session(cfg);
    } else {
      path_ = opt.file;
      buffer_.resize(std::size_t{8} << 20U);
    }
  }

  template <class Fn>
  bool for_each_chunk(Fn&& fn) {
    if (path_.empty()) {
      fn(std::span<const std::byte>{memory_});
      return true;
    }
    std::ifstream in(path_, std::ios::binary);
    if (!in) {
      return false;
    }
    std::size_t filled = 0;
    while (true) {
      in.read(reinterpret_cast<char*>(buffer_.data() + filled),  // NOLINT(*-reinterpret-cast)
              static_cast<std::streamsize>(buffer_.size() - filled));
      const auto got = static_cast<std::size_t>(in.gcount());
      if (got == 0) {
        return true;
      }
      filled += got;
      const std::size_t used = fn(std::span<const std::byte>{buffer_.data(), filled});
      std::memmove(buffer_.data(), buffer_.data() + used, filled - used);
      filled -= used;
    }
  }

  [[nodiscard]] std::string describe() const {
    if (path_.empty()) {
      return "synthetic session, " + std::to_string(memory_.size()) + " bytes in memory";
    }
    return path_;
  }

 private:
  std::vector<std::byte> memory_;
  std::string path_;
  std::vector<std::byte> buffer_;
};

struct PassResult {
  std::uint64_t messages = 0;
  std::uint64_t applied = 0;
  std::uint64_t rejected = 0;
  double seconds = 0;
};

template <class Book>
std::unique_ptr<Book> make_book(const Options& opt) {
  if constexpr (std::is_same_v<Book, OrderBook>) {
    OrderBook::Config c;
    c.max_orders = opt.max_orders;
    c.max_levels = opt.max_orders / 4;
    return std::make_unique<OrderBook>(c);
  } else {
    return std::make_unique<Book>();
  }
}

template <class Book>
PassResult throughput_pass(Source& src, const Options& opt) {
  auto book = make_book<Book>(opt);
  ItchBookBuilder builder(*book);
  PassResult r;
  const auto t0 = Clock::now();
  src.for_each_chunk([&](std::span<const std::byte> chunk) {
    const auto s = itch::parse_stream(chunk, builder);
    r.messages += s.messages;
    return s.bytes_consumed;
  });
  r.seconds = std::chrono::duration<double>(Clock::now() - t0).count();
  r.applied = builder.applied();
  r.rejected = builder.rejected();
  return r;
}

template <class Book>
PassResult latency_pass(Source& src, const Options& opt, Histogram& hist) {
  auto book = make_book<Book>(opt);
  ItchBookBuilder builder(*book);
  PassResult r;
  const auto t0 = Clock::now();
  src.for_each_chunk([&](std::span<const std::byte> chunk) {
    std::size_t pos = 0;
    while (chunk.size() - pos >= itch::kLengthPrefixSize) {
      const auto len = static_cast<std::size_t>((std::to_integer<unsigned>(chunk[pos]) << 8U) |
                                                std::to_integer<unsigned>(chunk[pos + 1]));
      if (chunk.size() - pos - itch::kLengthPrefixSize < len) {
        break;
      }
      const auto body = chunk.subspan(pos + itch::kLengthPrefixSize, len);
      const auto a = Clock::now();
      const auto err = itch::parse_message(body, builder);
      const auto b = Clock::now();
      hist.add(static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count()));
      r.messages += err == itch::ParseError::kOk ? 1U : 0U;
      pos += itch::kLengthPrefixSize + len;
    }
    return pos;
  });
  r.seconds = std::chrono::duration<double>(Clock::now() - t0).count();
  r.applied = builder.applied();
  r.rejected = builder.rejected();
  return r;
}

std::uint64_t timer_overhead_ns() {
  Histogram h;
  for (int i = 0; i < 1'000'000; ++i) {
    const auto a = Clock::now();
    const auto b = Clock::now();
    h.add(static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count()));
  }
  return h.percentile(0.5);
}

template <class Book>
int run(Source& src, const Options& opt) {
  std::printf("input   : %s\n", src.describe().c_str());
  std::printf("book    : %s\n", opt.reference ? "ReferenceBook (std::map)" : "OrderBook (optimized)");
  std::printf("runs    : %d (+1 warm-up)\n", opt.runs);
  std::printf("timer   : steady_clock, median back-to-back overhead %llu ns (not subtracted)\n\n",
              static_cast<unsigned long long>(timer_overhead_ns()));

  (void)throughput_pass<Book>(src, opt);  // warm-up: page cache, branch predictors

  std::vector<double> rates;
  Histogram hist;
  PassResult last;
  for (int i = 0; i < opt.runs; ++i) {
    last = throughput_pass<Book>(src, opt);
    rates.push_back(static_cast<double>(last.messages) / last.seconds);
    std::printf("run %d   : %.3f s  %.2f M msgs/s\n", i + 1, last.seconds, rates.back() / 1e6);
    (void)latency_pass<Book>(src, opt, hist);
  }
  std::sort(rates.begin(), rates.end());

  std::printf("\nmessages          : %llu  (book events applied %llu, rejected %llu)\n",
              static_cast<unsigned long long>(last.messages),
              static_cast<unsigned long long>(last.applied),
              static_cast<unsigned long long>(last.rejected));
  std::printf("throughput median : %.2f M msgs/s  (min %.2f, max %.2f)\n",
              rates[rates.size() / 2] / 1e6, rates.front() / 1e6, rates.back() / 1e6);
  std::printf("latency per msg   : p50 %llu ns | p90 %llu ns | p99 %llu ns | p99.9 %llu ns | max %llu ns\n",
              static_cast<unsigned long long>(hist.percentile(0.50)),
              static_cast<unsigned long long>(hist.percentile(0.90)),
              static_cast<unsigned long long>(hist.percentile(0.99)),
              static_cast<unsigned long long>(hist.percentile(0.999)),
              static_cast<unsigned long long>(hist.max()));
  return EXIT_SUCCESS;
}

}  // namespace

int main(int argc, char** argv) {
  Options opt;
  for (int i = 1; i < argc; ++i) {
    const std::string_view a = argv[i];
    auto next = [&]() -> const char* {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "missing value for %s\n", argv[i]);
        std::exit(EXIT_FAILURE);
      }
      return argv[++i];
    };
    if (a == "--file") {
      opt.file = next();
    } else if (a == "--synthetic") {
      opt.synthetic = std::strtoull(next(), nullptr, 10);
    } else if (a == "--runs") {
      opt.runs = std::max(1, std::atoi(next()));
    } else if (a == "--book") {
      opt.reference = std::string_view{next()} == "ref";
    } else if (a == "--max-orders") {
      opt.max_orders = std::strtoull(next(), nullptr, 10);
    } else {
      std::fprintf(stderr,
                   "usage: %s [--file F | --synthetic N] [--runs N] [--book fast|ref] "
                   "[--max-orders N]\n",
                   argv[0]);
      return EXIT_FAILURE;
    }
  }
  if (opt.file.empty() && opt.synthetic == 0) {
    opt.synthetic = 5'000'000;
  }

  Source src(opt);
  return opt.reference ? run<ReferenceBook>(src, opt) : run<OrderBook>(src, opt);
}
