// Benchmark: sflat::flat_hash_map vs emhash8::HashMap vs
// boost::unordered_flat_map vs boost::unordered_map vs std::unordered_map.
//
// Each scenario runs in a forked child so the parent can read the child's
// peak RSS (ru_maxrss) via wait4() for an honest per-map memory number.
//
// Scenarios (uint64_t -> uint64_t, random keys):
//   A: reserve(N) + insert N, find N hits, find N misses, erase N/2,
//      full iteration.  Map is kept alive at exit -> peak RSS ~= map memory.
//   B: insert N with NO reserve (exercises growth/rehash policy).
//   baseline: only the key/value/miss arrays, no map (subtracted from A).

#include <sflat/flat_hash_map.hpp>

#include <emhash/hash_table8.hpp>

#include <boost/unordered/unordered_flat_map.hpp>
#include <boost/unordered/unordered_map.hpp>

#include <unordered_map>

#ifdef HAS_ABSL
#include <absl/container/flat_hash_map.h>
#endif

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <random>
#include <vector>

namespace {

uint64_t splitmix64(uint64_t& s) {
  uint64_t z = (s += 0x9E3779B97F4A7C15ULL);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
  return z ^ (z >> 31);
}

using Clock = std::chrono::steady_clock;
template <typename F>
double ns_per_op(F&& f, size_t ops) {
  const auto t0 = Clock::now();
  f();
  const auto t1 = Clock::now();
  return std::chrono::duration<double, std::nano>(t1 - t0).count() /
         static_cast<double>(ops ? ops : 1);
}

// ---- per-map adapters -------------------------------------------------------
template <typename Map>
struct Ops {
  static void do_reserve(Map& m, size_t n) { m.reserve(n); }
  static void do_emplace(Map& m, uint64_t k, uint64_t v) { m.emplace(k, v); }
  static typename Map::iterator do_find(Map& m, uint64_t k) {
    return m.find(k);
  }
  static size_t do_erase(Map& m, uint64_t k) { return m.erase(k); }
};

template <>
struct Ops<emhash8::HashMap<uint64_t, uint64_t>> {
  using Map = emhash8::HashMap<uint64_t, uint64_t>;
  static void do_reserve(Map& m, size_t n) {
    m.reserve(static_cast<uint64_t>(n), false);
  }
  static void do_emplace(Map& m, uint64_t k, uint64_t v) { m.emplace(k, v); }
  static typename Map::iterator do_find(Map& m, uint64_t k) {
    return m.find(k);
  }
  static size_t do_erase(Map& m, uint64_t k) { return m.erase(k); }
};

// shuffled: the keys in a random order (part of the baseline memory).
void gen_arrays(size_t n, std::vector<uint64_t>& keys,
                std::vector<uint64_t>& vals, std::vector<uint64_t>& miss,
                std::vector<uint64_t>& shuffled) {
  keys.resize(n);
  vals.resize(n);
  miss.resize(n);
  uint64_t s = 0x123456789ABCDEFULL;
  for (size_t i = 0; i < n; ++i) {
    keys[i] = splitmix64(s);
    vals[i] = splitmix64(s);
  }
  s = 0xFEDCBA9876543210ULL;
  for (size_t i = 0; i < n; ++i) miss[i] = splitmix64(s);
  shuffled = keys;
  std::shuffle(shuffled.begin(), shuffled.end(), std::mt19937_64(42));
}

// Scenario A: reserved insert + finds + erase + iteration. Map stays alive.
template <typename Map>
void child_A(const char* name, size_t n) {
  std::vector<uint64_t> keys, vals, miss, shuffled;
  gen_arrays(n, keys, vals, miss, shuffled);
  volatile uint64_t sink = 0;

  Map m;
  const double t_insert = ns_per_op(
      [&] {
        Ops<Map>::do_reserve(m, n);
        for (size_t i = 0; i < n; ++i) Ops<Map>::do_emplace(m, keys[i], vals[i]);
      },
      n);
  const size_t after_insert = m.size();

  const double t_hit = ns_per_op(
      [&] {
        uint64_t acc = 0;
        for (size_t i = 0; i < n; ++i) {
          auto it = Ops<Map>::do_find(m, keys[i]);
          if (it != m.end()) acc += it->second;
        }
        sink = acc;
      },
      n);

  // Hits in a random order. Insertion order favors maps that store the
  // elements densely in insertion order (emhash8): the element reads are then
  // sequential and the hardware prefetcher hides them.
  const double t_hit_rand = ns_per_op(
      [&] {
        uint64_t acc = 0;
        for (size_t i = 0; i < n; ++i) {
          auto it = Ops<Map>::do_find(m, shuffled[i]);
          if (it != m.end()) acc += it->second;
        }
        sink = acc;
      },
      n);

  const double t_miss = ns_per_op(
      [&] {
        size_t found = 0;
        for (size_t i = 0; i < n; ++i)
          if (Ops<Map>::do_find(m, miss[i]) != m.end()) ++found;
        sink = found;  // expect 0
      },
      n);

  const double t_erase = ns_per_op(
      [&] {
        for (size_t i = 0; i < n; i += 2) Ops<Map>::do_erase(m, keys[i]);
      },
      n / 2);
  const size_t after_erase = m.size();

  const double t_iter = ns_per_op(
      [&] {
        uint64_t acc = 0;
        size_t cnt = 0;
        for (auto& kv : m) {
          acc += kv.second;
          ++cnt;
        }
        sink = acc + cnt;
      },
      after_erase);

  // keep map alive; peak RSS is captured by the parent via wait4
  std::printf(
      "A %-10s n=%-9zu insert=%7.1f find_hit=%7.1f find_hit_rand=%7.1f "
      "find_miss=%7.1f erase=%7.1f iter=%7.1f ns/op  size=%zu->%zu sink=%llu\n",
      name, n, t_insert, t_hit, t_hit_rand, t_miss, t_erase, t_iter, after_insert,
      after_erase, (unsigned long long)sink);
  std::fflush(stdout);
}

// Scenario B: insert with NO reserve (growth policy under test).
template <typename Map>
void child_B(const char* name, size_t n) {
  std::vector<uint64_t> keys, vals, miss, shuffled;
  gen_arrays(n, keys, vals, miss, shuffled);
  Map m;
  const double t_insert = ns_per_op(
      [&] {
        for (size_t i = 0; i < n; ++i) Ops<Map>::do_emplace(m, keys[i], vals[i]);
      },
      n);
  // touch everything once so the memory is really committed
  volatile uint64_t acc = 0;
  for (size_t i = 0; i < n; ++i) {
    auto it = Ops<Map>::do_find(m, keys[i]);
    if (it != m.end()) acc += it->second;
  }
  std::printf("B %-10s n=%-9zu insert_noreserve=%7.1f ns/op  buckets=%zu sink=%llu\n",
              name, n, t_insert, (size_t)m.bucket_count(),
              (unsigned long long)acc);
  std::fflush(stdout);
}

// Touch (and free) memory once before the runs. In a VM the host backs guest
// memory on first use, which is much slower than a normal page fault. Without
// this step the first map in the run order pays that cost.
void child_warmup(size_t bytes) {
  const long pages = sysconf(_SC_PHYS_PAGES);
  const long psize = sysconf(_SC_PAGE_SIZE);
  if (pages > 0 && psize > 0) {
    const size_t cap = static_cast<size_t>(pages) * psize / 4 * 3;
    if (bytes > cap) bytes = cap;
  }
  std::vector<char> buf(bytes);
  for (size_t i = 0; i < bytes; i += 4096) buf[i] = 1;
  volatile char c = buf[bytes / 2];
  (void)c;
}

void child_baseline(size_t n) {
  std::vector<uint64_t> keys, vals, miss, shuffled;
  gen_arrays(n, keys, vals, miss, shuffled);
  volatile uint64_t acc =
      keys[n - 1] + vals[n - 1] + miss[n - 1] + shuffled[n - 1];
  (void)acc;
}

// Run fn() in a forked child; return child's peak RSS in KiB.
long run_child(void (*fn)(void)) {
  std::fflush(stdout);  // don't let the child re-emit parent's buffered output
  pid_t pid = fork();
  if (pid == 0) {
    fn();
    _exit(0);
  }
  int status = 0;
  struct rusage ru;
  memset(&ru, 0, sizeof(ru));
  while (wait4(pid, &status, 0, &ru) < 0) {
  }
  return ru.ru_maxrss;  // KiB on Linux
}

// ---- instantiations ----------------------------------------------------------
using SflatMap = sflat::flat_hash_map<uint64_t, uint64_t>;
using EmhashMap = emhash8::HashMap<uint64_t, uint64_t>;
using BoostMap = boost::unordered_flat_map<uint64_t, uint64_t>;
using BoostNodeMap = boost::unordered_map<uint64_t, uint64_t>;
using StdMap = std::unordered_map<uint64_t, uint64_t>;
#ifdef HAS_ABSL
using AbslMap = absl::flat_hash_map<uint64_t, uint64_t>;
#endif

void run_A_sflat(size_t n) { child_A<SflatMap>("sflat", n); }
void run_A_emhash(size_t n) { child_A<EmhashMap>("emhash8", n); }
void run_A_boost(size_t n) { child_A<BoostMap>("boost_flat", n); }
void run_A_boost_node(size_t n) { child_A<BoostNodeMap>("boost_node", n); }
void run_A_std(size_t n) { child_A<StdMap>("std_unordered", n); }
#ifdef HAS_ABSL
void run_A_absl(size_t n) { child_A<AbslMap>("absl_flat", n); }
#endif
void run_B_sflat(size_t n) { child_B<SflatMap>("sflat", n); }
void run_B_emhash(size_t n) { child_B<EmhashMap>("emhash8", n); }
void run_B_boost(size_t n) { child_B<BoostMap>("boost_flat", n); }
void run_B_boost_node(size_t n) { child_B<BoostNodeMap>("boost_node", n); }
void run_B_std(size_t n) { child_B<StdMap>("std_unordered", n); }
#ifdef HAS_ABSL
void run_B_absl(size_t n) { child_B<AbslMap>("absl_flat", n); }
#endif

}  // namespace

int main(int argc, char** argv) {
  std::vector<size_t> sizes;
  for (int i = 1; i < argc; ++i) sizes.push_back(strtoull(argv[i], nullptr, 10));
  if (sizes.empty()) {
    sizes = {1000000, 5000000};
  }

  std::printf("# maps: sflat (1.5x growth, mlf=0.875, AVX2 groups) vs "
              "emhash8 (mlf=0.80) vs boost::unordered_flat_map vs "
              "boost::unordered_map vs std::unordered_map\n");
  std::printf("# cpu: x86-64, flags: -O3 -march=native -std=c++17\n");
  std::printf("# times are ns/op; rss values are child peak RSS in MiB\n\n");

  for (size_t n : sizes) {
    std::printf("=== n = %zu ===\n", n);
    // baseline (arrays only)
    struct Ctx {
      size_t n;
    };
    // use small trampolines via statics
    static size_t g_n = 0;
    g_n = n;
    run_child([] { child_warmup(g_n * 80); });
    const long rss_base =
        run_child([] { child_baseline(g_n); });
    const long rss_sflat =
        run_child([] { run_A_sflat(g_n); });
    const long rss_emhash =
        run_child([] { run_A_emhash(g_n); });
    const long rss_boost =
        run_child([] { run_A_boost(g_n); });
    const long rss_boost_node =
        run_child([] { run_A_boost_node(g_n); });
    const long rss_std =
        run_child([] { run_A_std(g_n); });
#ifdef HAS_ABSL
    const long rss_absl =
        run_child([] { run_A_absl(g_n); });
#endif
    std::printf(
        "  peak_rss MiB: baseline=%.1f sflat=%.1f emhash8=%.1f boost_flat=%.1f boost_node=%.1f std=%.1f"
#ifdef HAS_ABSL
        " absl=%.1f"
#endif
        "\n",
        rss_base / 1024.0, rss_sflat / 1024.0, rss_emhash / 1024.0,
        rss_boost / 1024.0, rss_boost_node / 1024.0, rss_std / 1024.0
#ifdef HAS_ABSL
        , rss_absl / 1024.0
#endif
        );
    std::printf(
        "  map-only MiB (minus baseline): sflat=%.1f emhash8=%.1f "
        "boost_flat=%.1f boost_node=%.1f std=%.1f"
#ifdef HAS_ABSL
        " absl=%.1f"
#endif
        "  => bytes/entry: %.1f / %.1f / %.1f / %.1f / %.1f"
#ifdef HAS_ABSL
        " / %.1f"
#endif
        "\n",
        (rss_sflat - rss_base) / 1024.0, (rss_emhash - rss_base) / 1024.0,
        (rss_boost - rss_base) / 1024.0, (rss_boost_node - rss_base) / 1024.0,
        (rss_std - rss_base) / 1024.0
#ifdef HAS_ABSL
        , (rss_absl - rss_base) / 1024.0
#endif
        ,
        (rss_sflat - rss_base) * 1024.0 / n, (rss_emhash - rss_base) * 1024.0 / n,
        (rss_boost - rss_base) * 1024.0 / n, (rss_boost_node - rss_base) * 1024.0 / n,
        (rss_std - rss_base) * 1024.0 / n
#ifdef HAS_ABSL
        , (rss_absl - rss_base) * 1024.0 / n
#endif
        );
    // scenario B (no reserve). Peak RSS here includes the transient peak
    // of growth (old table + new table alive at the same time).
    const long rssB_sflat = run_child([] { run_B_sflat(g_n); });
    const long rssB_emhash = run_child([] { run_B_emhash(g_n); });
    const long rssB_boost = run_child([] { run_B_boost(g_n); });
    const long rssB_boost_node = run_child([] { run_B_boost_node(g_n); });
    const long rssB_std = run_child([] { run_B_std(g_n); });
#ifdef HAS_ABSL
    const long rssB_absl = run_child([] { run_B_absl(g_n); });
#endif
    std::printf(
        "  B growth peak bytes/entry (minus baseline): sflat=%.1f "
        "emhash8=%.1f boost_flat=%.1f boost_node=%.1f std=%.1f"
#ifdef HAS_ABSL
        " absl=%.1f"
#endif
        "\n",
        (rssB_sflat - rss_base) * 1024.0 / n,
        (rssB_emhash - rss_base) * 1024.0 / n,
        (rssB_boost - rss_base) * 1024.0 / n,
        (rssB_boost_node - rss_base) * 1024.0 / n,
        (rssB_std - rss_base) * 1024.0 / n
#ifdef HAS_ABSL
        , (rssB_absl - rss_base) * 1024.0 / n
#endif
        );
    std::printf("\n");
    std::fflush(stdout);
  }
  return 0;
}
