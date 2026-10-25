#include "matchcore/itch/synthetic.hpp"

#include <algorithm>
#include <random>
#include <string>
#include <string_view>

namespace matchcore::itch {

namespace {

class Writer {
 public:
  explicit Writer(std::vector<std::byte>& out) : out_(out) {}

  void begin(std::size_t body_len) { be(body_len, 2); }
  void ch(char c) { out_.push_back(static_cast<std::byte>(c)); }
  void be(std::uint64_t v, std::size_t width) {
    for (std::size_t i = width; i > 0; --i) {
      out_.push_back(static_cast<std::byte>((v >> (8U * (i - 1))) & 0xFFU));
    }
  }
  void alpha(std::string_view s, std::size_t width) {
    for (std::size_t i = 0; i < width; ++i) {
      ch(i < s.size() ? s[i] : ' ');
    }
  }
  void header(char type, std::uint16_t locate, std::uint64_t ts) {
    ch(type);
    be(locate, 2);
    be(0, 2);  // tracking number
    be(ts, 6);
  }
 private:
  std::vector<std::byte>& out_;
};

struct Live {
  std::uint64_t id;
  std::uint32_t price;
  std::uint32_t shares;
  std::uint16_t locate;
  char side;
};

std::string symbol_name(std::uint16_t locate) {
  // SYM0001, SYM0002, ...
  std::string s = "SYM";
  const std::string n = std::to_string(locate);
  s.append(4 - std::min<std::size_t>(4, n.size()), '0');
  s += n;
  return s;
}

}  // namespace

std::vector<std::byte> generate_session(const SyntheticConfig& cfg) {
  std::vector<std::byte> out;
  out.reserve(cfg.messages * 32 + std::size_t{cfg.symbols} * 41 + 64);
  Writer w(out);
  std::mt19937_64 rng(cfg.seed);

  std::uint64_t ts = 4ULL * 3600 * 1'000'000'000;  // 04:00:00, pre-market open
  auto tick_time = [&] { ts += 1 + rng() % 2000; };

  w.begin(12);
  w.header('S', 0, ts);
  w.ch('O');

  for (std::uint16_t loc = 1; loc <= cfg.symbols; ++loc) {
    tick_time();
    w.begin(39);
    w.header('R', loc, ts);
    w.alpha(symbol_name(loc), 8);
    w.ch('Q');
    w.ch('N');
    w.be(100, 4);
    w.ch('N');
    w.ch('C');
    w.alpha("", 2);
    w.ch('P');
    w.ch('N');
    w.ch(' ');
    w.ch('1');
    w.ch('N');
    w.be(0, 4);
    w.ch('N');
  }

  // Fixed per-symbol mid prices, spread around base_price so symbols differ.
  std::vector<std::uint32_t> mid(std::size_t{cfg.symbols} + 1, cfg.base_price);
  for (std::size_t loc = 1; loc < mid.size(); ++loc) {
    const auto spread_ticks = static_cast<std::uint32_t>(rng() % 2001);  // +/- 10.00
    mid[loc] = std::max(cfg.base_price + spread_ticks * cfg.tick - 1000 * cfg.tick,
                        cfg.tick * (cfg.price_band_ticks + 10));
  }
  std::vector<Live> live;
  live.reserve(cfg.target_resting * 2);
  std::uint64_t next_id = 1;
  std::uint64_t match = 1;

  const unsigned total_w = cfg.w_add + cfg.w_delete + cfg.w_execute + cfg.w_cancel + cfg.w_replace;

  auto rand_price = [&](std::uint16_t loc, char side) {
    // Skew toward the inside: |offset| is the min of two uniform draws.
    const auto band = cfg.price_band_ticks;
    const auto a = static_cast<std::uint32_t>(rng() % (band + 1));
    const auto b = static_cast<std::uint32_t>(rng() % (band + 1));
    const std::uint32_t off = (1 + std::min(a, b)) * cfg.tick;
    const std::uint32_t m = mid[loc];
    // Bids strictly below mid, asks strictly above, and the mid never moves:
    // the book can never cross. (A real exchange book is never crossed
    // either, because crossing orders execute instead of resting.)
    if (side == 'B') {
      return m > off + cfg.tick ? m - off : cfg.tick;
    }
    return m + off;
  };

  for (std::size_t n = 0; n < cfg.messages; ++n) {
    tick_time();

    // Bias toward adds when shallow and removals when deep.
    unsigned roll = static_cast<unsigned>(rng() % total_w);
    if (live.empty() || (live.size() < cfg.target_resting / 2 && roll >= cfg.w_add)) {
      roll = 0;  // force add
    } else if (live.size() > cfg.target_resting * 2 && roll < cfg.w_add) {
      roll = cfg.w_add;  // force delete
    }

    if (roll < cfg.w_add) {
      const auto loc = static_cast<std::uint16_t>(1 + rng() % cfg.symbols);
      const char side = (rng() & 1U) != 0 ? 'B' : 'S';
      const std::uint32_t px = rand_price(loc, side);
      const auto shares = static_cast<std::uint32_t>(100 * (1 + rng() % 10));
      const std::uint64_t id = next_id++;
      const bool mpid = rng() % 20 == 0;
      w.begin(mpid ? 40 : 36);
      w.header(mpid ? 'F' : 'A', loc, ts);
      w.be(id, 8);
      w.ch(side);
      w.be(shares, 4);
      w.alpha(symbol_name(loc), 8);
      w.be(px, 4);
      if (mpid) {
        w.alpha("MCOR", 4);
      }
      live.push_back({id, px, shares, loc, side});
      continue;
    }

    const std::size_t pick = rng() % live.size();
    Live& o = live[pick];
    auto erase_pick = [&] {
      live[pick] = live.back();
      live.pop_back();
    };
    roll -= cfg.w_add;

    if (roll < cfg.w_delete) {
      w.begin(19);
      w.header('D', o.locate, ts);
      w.be(o.id, 8);
      erase_pick();
    } else if ((roll -= cfg.w_delete) < cfg.w_execute) {
      const auto qty = static_cast<std::uint32_t>(1 + rng() % o.shares);
      const bool with_price = rng() % 10 == 0;
      w.begin(with_price ? 36 : 31);
      w.header(with_price ? 'C' : 'E', o.locate, ts);
      w.be(o.id, 8);
      w.be(qty, 4);
      w.be(match++, 8);
      if (with_price) {
        w.ch('Y');
        w.be(o.price, 4);
      }
      o.shares -= qty;
      if (o.shares == 0) {
        erase_pick();
      }
    } else if ((roll -= cfg.w_execute) < cfg.w_cancel) {
      if (o.shares < 2) {
        w.begin(19);
        w.header('D', o.locate, ts);
        w.be(o.id, 8);
        erase_pick();
      } else {
        const auto qty = static_cast<std::uint32_t>(1 + rng() % (o.shares - 1));
        w.begin(23);
        w.header('X', o.locate, ts);
        w.be(o.id, 8);
        w.be(qty, 4);
        o.shares -= qty;
      }
    } else {
      const std::uint64_t nid = next_id++;
      const std::uint32_t px = rand_price(o.locate, o.side);
      const auto shares = static_cast<std::uint32_t>(100 * (1 + rng() % 10));
      w.begin(35);
      w.header('U', o.locate, ts);
      w.be(o.id, 8);
      w.be(nid, 8);
      w.be(shares, 4);
      w.be(px, 4);
      o.id = nid;
      o.price = px;
      o.shares = shares;
    }
  }

  tick_time();
  w.begin(12);
  w.header('S', 0, ts);
  w.ch('C');
  return out;
}

}  // namespace matchcore::itch
