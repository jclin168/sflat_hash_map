// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jian-Cheng Lin
// Octave churn: erase one key and insert one new key, at a fixed size.
//
// The workload and the method follow martinus/unordered_dense,
// scripts/ab/maps.{h,cpp} (MIT license): the same key pools and churn loop,
// nanobench's compare() to run the maps interleaved (so drift of the machine
// cancels out of the ratios), and sizes log-spaced over one octave with the
// geometric mean per octave. One size alone is a point on each map's load
// factor sawtooth, and the maps grow at different sizes. 50 points per
// octave is what a comparison between libraries needs (see that project's
// scripts/ab/README.md).
//
// Each map uses its own default hash. After the churn the program also
// measures lookups (half hits) on the churned tables.
//
// Build: needs nanobench.h with targetIntervalWidth() (the copy in
// unordered_dense/test/third-party), Boost, and optionally Abseil
// (-DHAS_ABSL). -DSFLAT_BASE adds <sflatbase/flat_hash_map.hpp>, a copy of an
// older sflat header renamed to namespace sflatbase, for an A/B run.
//
//   POINTS=50 ./churn 1000 32000 500000
//   (env: POINTS, default 50; INTERVAL, default 0.03; EPOCHS, default 200)
#include <sflat/flat_hash_map.hpp>
#ifdef SFLAT_BASE
#include <sflatbase/flat_hash_map.hpp>
#endif
#include <boost/unordered/unordered_flat_map.hpp>
#ifdef HAS_ABSL
#include <absl/container/flat_hash_map.h>
#endif
#define ANKERL_NANOBENCH_IMPLEMENT
#include <third-party/nanobench.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <tuple>
#include <utility>
#include <vector>

namespace {

template <typename M, const char* N>
struct Entry {
  static constexpr const char* name = N;
  M m;
};
constexpr char kSflat[] = "sflat";
constexpr char kBase[] = "sflat_base";
constexpr char kBoost[] = "boost";
constexpr char kAbsl[] = "absl";
using Tuple = std::tuple<
    Entry<sflat::flat_hash_map<uint64_t, size_t>, kSflat>
#ifdef SFLAT_BASE
    , Entry<sflatbase::flat_hash_map<uint64_t, size_t>, kBase>
#endif
    , Entry<boost::unordered_flat_map<uint64_t, size_t>, kBoost>
#ifdef HAS_ABSL
    , Entry<absl::flat_hash_map<uint64_t, size_t>, kAbsl>
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

// Erase one, insert one, holding the size exactly where it is. The new key
// is one the map has never held.
template <typename Map>
void churn(Map& m, std::vector<uint64_t>& present, std::vector<uint64_t>& spare,
           ankerl::nanobench::Rng& rng, size_t ops, size_t& tick) {
  for (size_t i = 0; i < ops; ++i) {
    const size_t slot =
        static_cast<size_t>(((rng() >> 32U) * present.size()) >> 32U);
    const size_t j = tick++ % spare.size();
    m.erase(present[slot]);
    m.try_emplace(spare[j], 1);
    std::swap(present[slot], spare[j]);
  }
}

template <typename Map>
size_t lookups(const Map& m, const std::vector<uint64_t>& present,
               const std::vector<uint64_t>& absent, ankerl::nanobench::Rng& rng,
               ankerl::nanobench::Rng& coin, size_t n) {
  size_t acc = 0;
  for (size_t i = 0; i < n; ++i) {
    const size_t at =
        static_cast<size_t>(((rng() >> 32U) * present.size()) >> 32U);
    acc += m.count((coin() & 1U) ? present[at] : absent[at]);
  }
  return acc;
}

template <typename F, size_t... I>
auto compare_all(ankerl::nanobench::Bench& b, F&& f, std::index_sequence<I...>) {
  auto args = std::tuple_cat(std::make_tuple(
      std::tuple_element_t<I, Tuple>::name,
      f(std::integral_constant<size_t, I>{}))...);
  return std::apply([&](auto&&... a) { return b.compare(a...); }, args);
}

template <size_t... I>
std::vector<const char*> names(std::index_sequence<I...>) {
  return {std::tuple_element_t<I, Tuple>::name...};
}

}  // namespace

int main(int argc, char** argv) {
  size_t points = 50, epochs = 200;
  double interval = 0.03;
  if (const char* e = std::getenv("POINTS")) points = std::strtoul(e, nullptr, 10);
  if (const char* e = std::getenv("INTERVAL")) interval = std::strtod(e, nullptr);
  if (const char* e = std::getenv("EPOCHS")) epochs = std::strtoul(e, nullptr, 10);
  std::vector<size_t> bases;
  for (int i = 1; i < argc; ++i)
    bases.push_back(std::strtoull(argv[i], nullptr, 10));
  if (bases.empty()) bases = {1000, 32000, 500000};
  const auto name = names(std::make_index_sequence<K>{});
  const size_t batch = 20000;

  for (size_t base : bases) {
    std::vector<double> log_churn(K, 0.0), log_find(K, 0.0);
    std::printf("== churn octave starting at %zu, %zu points ==\n", base,
                points);
    for (size_t p = 0; p < points; ++p) {
      const size_t n = static_cast<size_t>(
          static_cast<double>(base) *
          std::pow(2.0, static_cast<double>(p) / static_cast<double>(points)));
      const Pools pools(n);
      Tuple ms;  // no reserve: the load factor follows each map's sawtooth
      std::apply([&](auto&... e) { ((void)[&] { for (uint64_t k : pools.present) e.m.try_emplace(k, 1); }(), ...); }, ms);
      std::vector<std::vector<uint64_t>> present(K, pools.present);
      std::vector<std::vector<uint64_t>> spare(K, pools.spare);
      std::vector<ankerl::nanobench::Rng> rngs, lr, lc;
      for (size_t a = 0; a < K; ++a) {
        rngs.emplace_back(7);
        lr.emplace_back(5);
        lc.emplace_back(99);
      }
      std::vector<size_t> ticks(K, 0);
      auto bench = [&] {
        ankerl::nanobench::Bench b;
        b.batch(static_cast<double>(batch))
            .performanceCounters(false)
            .output(nullptr)
            .targetIntervalWidth(interval)
            .maxEpochs(epochs);
        return b;
      };
      auto b1 = bench();
      const auto r1 = compare_all(b1, [&](auto ic) {
        return [&] {
          churn(std::get<ic.value>(ms).m, present[ic.value], spare[ic.value],
                rngs[ic.value], batch, ticks[ic.value]);
        };
      }, std::make_index_sequence<K>{});
      auto b2 = bench();
      const auto r2 = compare_all(b2, [&](auto ic) {
        return [&] {
          ankerl::nanobench::doNotOptimizeAway(
              lookups(std::get<ic.value>(ms).m, present[ic.value],
                      pools.absent, lr[ic.value], lc[ic.value], batch));
        };
      }, std::make_index_sequence<K>{});
      std::printf("  n=%-8zu sflat load=%.3f", n,
                  static_cast<double>(n) /
                      static_cast<double>(std::get<0>(ms).m.bucket_count()));
      for (size_t a = 0; a < K; ++a) {
        using M = ankerl::nanobench::Result::Measure;
        const double c = r1[a].result.median(M::elapsed) * 1e9 / batch;
        const double f = r2[a].result.median(M::elapsed) * 1e9 / batch;
        log_churn[a] += std::log(c);
        log_find[a] += std::log(f);
        std::printf(" %s=%.2f/%.2f", name[a], c, f);
      }
      std::printf("\n");
      std::fflush(stdout);
    }
    std::printf("# churn  octave@%-8zu", base);
    for (size_t a = 0; a < K; ++a)
      std::printf(" %s=%.2f", name[a], std::exp(log_churn[a] / points));
    std::printf("\n# half   octave@%-8zu", base);
    for (size_t a = 0; a < K; ++a)
      std::printf(" %s=%.2f", name[a], std::exp(log_find[a] / points));
    std::printf("\n");
    std::fflush(stdout);
  }
  return 0;
}
