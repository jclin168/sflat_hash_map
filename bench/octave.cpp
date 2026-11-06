// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jian-Cheng Lin
// Octave benchmark: the workloads of Martin Ankerl's map comparison, for
// sflat and other flat hash maps.
//
// The workloads and the method follow martinus/unordered_dense,
// scripts/ab/maps.{h,cpp} (MIT license): the same three disjoint key pools,
// nanobench's compare() to run the maps interleaved (so drift of the machine
// cancels out of the ratios), and sizes log-spaced over one octave with the
// geometric mean per octave. One size alone is a point on each map's load
// factor sawtooth, and the maps grow at different sizes; a comparison
// between libraries wants many points per octave (that project uses 50).
//
// Workloads (time per operation):
//   build     insert n keys into an empty map, no reserve; includes the
//             destructor (per inserted key)
//   reserved  the same after reserve(n)
//   hit       lookups, all hits (count)
//   miss      lookups, all misses
//   half      lookups, half hits
//   bump      ++m[k] on keys that are in the map
//   iterate   sum of all mapped values (per element)
//   churn     erase a key that is in the map, insert one it never held; the
//             size stays n (per pair)
//   ie        ++m[k] on a present key, erase an absent key, erase a present
//             key, ++m[k] on a new key (per pair of operations)
//
// Each map uses its own default hash: what a caller gets by typing the type
// name. Maps are compiled in when their headers are found (-DHAS_ABSL,
// -DHAS_EMHASH, -DHAS_UDM, -DHAS_FOLLY); -DSFLAT_BASE adds
// <sflatbase/flat_hash_map.hpp>, an older sflat header renamed to namespace
// sflatbase, for an A/B run. Needs nanobench.h with targetIntervalWidth()
// (the copy in unordered_dense/test/third-party) and Boost.
//
//   ./octave [base...]          default bases 1000 32000 500000
//   env: ONLY=build,hit,...     the workloads to run (default all)
//        POINTS=50              sizes per octave
//        INTERVAL=0.03 EPOCHS=200   nanobench compare() precision and limit
#include <sflat/flat_hash_map.hpp>
#ifdef SFLAT_BASE
#include <sflatbase/flat_hash_map.hpp>
#endif
#include <boost/unordered/unordered_flat_map.hpp>
#ifdef HAS_ABSL
#include <absl/container/flat_hash_map.h>
#endif
#ifdef HAS_EMHASH
#include <emhash/hash_table8.hpp>
#endif
#ifdef HAS_UDM
#include <ankerl/unordered_dense.h>
#endif
#ifdef HAS_FOLLY
#include <folly/container/F14Map.h>
#endif
#define ANKERL_NANOBENCH_IMPLEMENT
#include <third-party/nanobench.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace {

// One interface for all maps. insert() must not overwrite (try_emplace).
template <typename Map, const char* N>
struct Adapter {
  static constexpr const char* name = N;
  Map m;
  void reserve(size_t n) { m.reserve(n); }
  void insert(uint64_t k, size_t v) { m.try_emplace(k, v); }
  size_t bump(uint64_t k) { return ++m[k]; }
  size_t count(uint64_t k) const { return static_cast<size_t>(m.count(k)); }
  void erase(uint64_t k) { m.erase(k); }
  size_t sum() const {
    size_t s = 0;
    for (const auto& kv : m) s += kv.second;
    return s;
  }
  size_t size() const { return m.size(); }
};

constexpr char kSflat[] = "sflat";
constexpr char kBase[] = "sflat_base";
constexpr char kBoost[] = "boost";
constexpr char kAbsl[] = "absl";
constexpr char kEmhash[] = "emhash8";
constexpr char kUdm[] = "udm";
constexpr char kF14[] = "f14";

#ifdef HAS_EMHASH
// emhash8::reserve(n) takes a bucket count; reserve(n, false) takes elements.
struct EmhashAdapter
    : Adapter<emhash8::HashMap<uint64_t, size_t>, kEmhash> {
  void reserve(size_t n) { m.reserve(static_cast<uint64_t>(n), false); }
};
#endif

using Tuple = std::tuple<
    Adapter<sflat::flat_hash_map<uint64_t, size_t>, kSflat>
#ifdef SFLAT_BASE
    , Adapter<sflatbase::flat_hash_map<uint64_t, size_t>, kBase>
#endif
    , Adapter<boost::unordered_flat_map<uint64_t, size_t>, kBoost>
#ifdef HAS_ABSL
    , Adapter<absl::flat_hash_map<uint64_t, size_t>, kAbsl>
#endif
#ifdef HAS_EMHASH
    , EmhashAdapter
#endif
#ifdef HAS_UDM
    , Adapter<ankerl::unordered_dense::map<uint64_t, size_t>, kUdm>
#endif
#ifdef HAS_FOLLY
    , Adapter<folly::F14ValueMap<uint64_t, size_t>, kF14>
#endif
    >;
constexpr size_t K = std::tuple_size_v<Tuple>;

// Three disjoint key pools, so a miss is absent by construction and churn
// never builds a key inside the timed region.
std::vector<uint64_t> make_keys(uint64_t range, size_t n, uint64_t seed) {
  std::vector<uint64_t> out;
  out.reserve(n);
  ankerl::nanobench::Rng r(seed);
  for (size_t i = 0; i < n; ++i) out.push_back((r() >> 2U) | range);
  return out;
}

struct Pools {
  std::vector<uint64_t> present, absent, spare;
  explicit Pools(size_t n)
      : present(make_keys(0, n, 1)),
        absent(make_keys(uint64_t{1} << 63, n, 2)),
        spare(make_keys(uint64_t{1} << 62, n, 3)) {}
};

template <typename A>
void fill(A& a, const Pools& p) {
  for (uint64_t k : p.present) a.insert(k, 1);
}

template <typename A>
size_t build(const Pools& p, bool reserve) {
  A a;
  if (reserve) a.reserve(p.present.size());
  fill(a, p);
  return a.size();
}

// The rngs outlive the epochs: a batch small enough to memorise, replayed,
// would be learned by the branch predictor.
struct LookupState {
  ankerl::nanobench::Rng rng{5};
  ankerl::nanobench::Rng coin{99};
};

enum class Asking { hits, half, misses };

template <typename A>
size_t lookups(const A& a, const Pools& p, LookupState& st, Asking what,
               size_t n) {
  size_t acc = 0;
  for (size_t i = 0; i < n; ++i) {
    const size_t at =
        static_cast<size_t>(((st.rng() >> 32U) * p.present.size()) >> 32U);
    const bool hit =
        what == Asking::hits || (what == Asking::half && (st.coin() & 1U));
    acc += a.count(hit ? p.present[at] : p.absent[at]);
  }
  return acc;
}

template <typename A>
size_t bump_present(A& a, const Pools& p, LookupState& st, size_t n) {
  size_t acc = 0;
  for (size_t i = 0; i < n; ++i) {
    const size_t at =
        static_cast<size_t>(((st.rng() >> 32U) * p.present.size()) >> 32U);
    acc += a.bump(p.present[at]);
  }
  return acc;
}

template <typename A>
void churn(A& a, std::vector<uint64_t>& present, std::vector<uint64_t>& spare,
           ankerl::nanobench::Rng& rng, size_t ops, size_t& tick) {
  for (size_t i = 0; i < ops; ++i) {
    const size_t slot =
        static_cast<size_t>(((rng() >> 32U) * present.size()) >> 32U);
    const size_t j = tick++ % spare.size();
    a.erase(present[slot]);
    a.insert(spare[j], 1);
    std::swap(present[slot], spare[j]);
  }
}

template <typename A>
size_t insert_erase(A& a, Pools& p, ankerl::nanobench::Rng& rng, size_t ops,
                    size_t& tick) {
  size_t acc = 0;
  for (size_t i = 0; i < ops; ++i) {
    const size_t x =
        static_cast<size_t>(((rng() >> 32U) * p.present.size()) >> 32U);
    acc += a.bump(p.present[x]);
    a.erase(p.absent[x]);
    const size_t y =
        static_cast<size_t>(((rng() >> 32U) * p.present.size()) >> 32U);
    const size_t j = tick++ % p.spare.size();
    a.erase(p.present[y]);
    acc += a.bump(p.spare[j]);
    std::swap(p.present[y], p.spare[j]);
  }
  return acc;
}

// ------------------------------------------------------------------ driver
template <size_t... I>
std::vector<const char*> names(std::index_sequence<I...>) {
  return {std::tuple_element_t<I, Tuple>::name...};
}

template <typename F, size_t... I>
auto compare_all(ankerl::nanobench::Bench& b, F&& f, std::index_sequence<I...>) {
  auto args = std::tuple_cat(std::make_tuple(
      std::tuple_element_t<I, Tuple>::name,
      f(std::integral_constant<size_t, I>{}))...);
  return std::apply([&](auto&&... a) { return b.compare(a...); }, args);
}

double g_interval = 0.03;
size_t g_epochs = 200;
size_t g_points = 50;
std::string g_only;

bool wanted(const char* w) {
  if (g_only.empty()) return true;
  const std::string s = "," + g_only + ",";
  return s.find("," + std::string(w) + ",") != std::string::npos;
}

struct Acc {
  std::vector<double> logsum = std::vector<double>(K, 0.0);
  std::vector<double> lo = std::vector<double>(K, 1e300);
  std::vector<double> hi = std::vector<double>(K, 0.0);
  size_t n = 0;
};

template <typename F>
void one_point(const char* what, size_t n, size_t batch, Acc& acc, F&& f) {
  ankerl::nanobench::Bench bench;
  bench.batch(static_cast<double>(batch))
      .performanceCounters(false)
      .output(nullptr)
      .targetIntervalWidth(g_interval)
      .maxEpochs(g_epochs);
  const auto res = compare_all(bench, f, std::make_index_sequence<K>{});
  static const auto name = names(std::make_index_sequence<K>{});
  std::printf("  %-8s n=%-8zu", what, n);
  for (size_t a = 0; a < K; ++a) {
    using M = ankerl::nanobench::Result::Measure;
    const double ns = res[a].result.median(M::elapsed) * 1e9 /
                      static_cast<double>(batch);
    acc.logsum[a] += std::log(ns);
    if (ns < acc.lo[a]) acc.lo[a] = ns;
    if (ns > acc.hi[a]) acc.hi[a] = ns;
    std::printf(" %s=%.2f", name[a], ns);
  }
  ++acc.n;
  std::printf("\n");
  std::fflush(stdout);
}

void print_geomean(const char* what, size_t base, const Acc& acc) {
  if (acc.n == 0) return;
  static const auto name = names(std::make_index_sequence<K>{});
  std::printf("# %-8s octave@%-8zu", what, base);
  for (size_t a = 0; a < K; ++a)
    std::printf(" %s=%.2f", name[a],
                std::exp(acc.logsum[a] / static_cast<double>(acc.n)));
  std::printf("   | max");
  for (size_t a = 0; a < K; ++a) std::printf(" %s=%.1f", name[a], acc.hi[a]);
  std::printf("\n");
  std::fflush(stdout);
}

template <typename F>
void each(Tuple& ms, F&& f) {
  std::apply([&](auto&... a) { (f(a), ...); }, ms);
}

void sweep(size_t base) {
  Acc a_build, a_res, a_hit, a_miss, a_half, a_bump, a_iter, a_churn, a_ie;
  std::printf("== octave starting at %zu, %zu points ==\n", base, g_points);
  for (size_t i = 0; i < g_points; ++i) {
    const size_t n = static_cast<size_t>(
        static_cast<double>(base) *
        std::pow(2.0, static_cast<double>(i) / static_cast<double>(g_points)));
    const Pools p(n);
    const size_t lookup_batch = 20000;

    if (wanted("build")) {
      one_point("build", n, n, a_build, [&](auto ic) {
        return [&] {
          ankerl::nanobench::doNotOptimizeAway(
              build<std::tuple_element_t<ic.value, Tuple>>(p, false));
        };
      });
    }
    if (wanted("reserved")) {
      one_point("reserved", n, n, a_res, [&](auto ic) {
        return [&] {
          ankerl::nanobench::doNotOptimizeAway(
              build<std::tuple_element_t<ic.value, Tuple>>(p, true));
        };
      });
    }
    if (wanted("hit") || wanted("miss") || wanted("half") || wanted("bump") ||
        wanted("iterate")) {
      Tuple ms;
      each(ms, [&](auto& a) { fill(a, p); });
      std::vector<LookupState> st(K);
      const struct {
        const char* label;
        Asking what;
        Acc* acc;
      } kinds[] = {{"hit", Asking::hits, &a_hit},
                   {"miss", Asking::misses, &a_miss},
                   {"half", Asking::half, &a_half}};
      for (const auto& kind : kinds) {
        if (!wanted(kind.label)) continue;
        one_point(kind.label, n, lookup_batch, *kind.acc, [&](auto ic) {
          return [&, what = kind.what] {
            ankerl::nanobench::doNotOptimizeAway(lookups(
                std::get<ic.value>(ms), p, st[ic.value], what, lookup_batch));
          };
        });
      }
      if (wanted("bump")) {
        one_point("bump", n, lookup_batch, a_bump, [&](auto ic) {
          return [&] {
            ankerl::nanobench::doNotOptimizeAway(bump_present(
                std::get<ic.value>(ms), p, st[ic.value], lookup_batch));
          };
        });
      }
      if (wanted("iterate")) {
        one_point("iterate", n, n, a_iter, [&](auto ic) {
          return [&] {
            ankerl::nanobench::doNotOptimizeAway(std::get<ic.value>(ms).sum());
          };
        });
      }
    }
    if (wanted("churn")) {
      Tuple ms;
      each(ms, [&](auto& a) { fill(a, p); });
      std::vector<std::vector<uint64_t>> present(K, p.present);
      std::vector<std::vector<uint64_t>> spare(K, p.spare);
      std::vector<ankerl::nanobench::Rng> rngs;
      for (size_t a = 0; a < K; ++a) rngs.emplace_back(7);
      std::vector<size_t> ticks(K, 0);
      const size_t batch = 20000;
      one_point("churn", n, batch, a_churn, [&](auto ic) {
        return [&] {
          churn(std::get<ic.value>(ms), present[ic.value], spare[ic.value],
                rngs[ic.value], batch, ticks[ic.value]);
        };
      });
    }
    if (wanted("ie")) {
      Tuple ms;
      each(ms, [&](auto& a) { fill(a, p); });
      std::vector<Pools> ps(K, p);
      std::vector<ankerl::nanobench::Rng> rngs;
      for (size_t a = 0; a < K; ++a) rngs.emplace_back(7);
      std::vector<size_t> ticks(K, 0);
      const size_t batch = 10000;
      one_point("ie", n, 2 * batch, a_ie, [&](auto ic) {
        return [&] {
          ankerl::nanobench::doNotOptimizeAway(insert_erase(
              std::get<ic.value>(ms), ps[ic.value], rngs[ic.value], batch,
              ticks[ic.value]));
        };
      });
    }
  }
  print_geomean("build", base, a_build);
  print_geomean("reserved", base, a_res);
  print_geomean("hit", base, a_hit);
  print_geomean("miss", base, a_miss);
  print_geomean("half", base, a_half);
  print_geomean("bump", base, a_bump);
  print_geomean("iterate", base, a_iter);
  print_geomean("churn", base, a_churn);
  print_geomean("ie", base, a_ie);
}

}  // namespace

int main(int argc, char** argv) {
  if (const char* e = std::getenv("POINTS")) g_points = std::strtoul(e, nullptr, 10);
  if (const char* e = std::getenv("INTERVAL")) g_interval = std::strtod(e, nullptr);
  if (const char* e = std::getenv("EPOCHS")) g_epochs = std::strtoul(e, nullptr, 10);
  if (const char* e = std::getenv("ONLY")) g_only = e;
  std::vector<size_t> bases;
  for (int i = 1; i < argc; ++i)
    bases.push_back(std::strtoull(argv[i], nullptr, 10));
  if (bases.empty()) bases = {1000, 32000, 500000};
  std::printf("# maps:");
  for (const char* n : names(std::make_index_sequence<K>{})) std::printf(" %s", n);
  std::printf("\n");
  for (size_t b : bases) sweep(b);
  return 0;
}
