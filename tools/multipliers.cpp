// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jian-Cheng Lin
// Searches and checks the two tables of the seeded mixer:
//   h = r ^ (r >> 64),  r = (x ^ T[j]) * M[i]   (128-bit product)
// where the seed of a table picks i and j: 64 x 64 = 4096 mixers.
//
// T: with one multiplier, the inputs of a key under T[a] and T[b] differ by
// one of 2^p values, p = bits of T[a] ^ T[b] inside the bits that vary
// between keys. Iterating one table then feeds the other 2^p sorted runs,
// which pile up for small p. So any two entries differ in enough
// bits in every window of consecutive bits (see kWindows).
//
// M: a multiplier is kept if
//  1. quality: with every T[j], keys with regular bits (sequential IDs,
//     aligned pointers, shifted IDs) spread over all H2 values, all overflow
//     bits and the groups, like random hashes do; and
//  2. pairs: iterating a table under it and inserting into a table under
//     each multiplier already kept, or under itself with another T[j], does
//     not pile up (see PairProbe).
//
// Build: g++ -std=c++17 -O2 -march=native -pthread -Iinclude tools/multipliers.cpp
// Run:   ./a.out [count [progress]]  prints both tables as C++; with the
//                              stderr of a stopped run, continues it with
//                              the same result
//        ./a.out --check       measures the golden-ratio multiplier alone
#include <sflat/flat_hash_map.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <thread>
#include <vector>

using sflat::detail::Fastrange;
using sflat::detail::H2;

static uint64_t Mix(uint64_t x, uint64_t t, uint64_t m) {
  const __uint128_t r = (__uint128_t)(x ^ t) * m;
  return (uint64_t)r ^ (uint64_t)(r >> 64);
}

using Pattern = uint64_t (*)(uint64_t);
static const Pattern kPatterns[] = {
    [](uint64_t i) { return i; },
    [](uint64_t i) { return i * 3; },
    [](uint64_t i) { return i * 1000; },
    [](uint64_t i) { return i * 12345; },
    [](uint64_t i) -> uint64_t { return 0x7f0000000000ull + i * 8; },
    [](uint64_t i) -> uint64_t { return 0x7f0000000000ull + i * 16; },
    [](uint64_t i) -> uint64_t { return 0x7f0000000000ull + i * 64; },
    [](uint64_t i) { return i << 8; },
    [](uint64_t i) { return i << 16; },
    [](uint64_t i) { return i << 32; },
    [](uint64_t i) { return i << 48; },
    [](uint64_t i) { return (i << 32) | i; },
};
static const char* const kPatternNames[] = {
    "i", "i*3", "i*1000", "i*12345", "ptr i*8", "ptr i*16", "ptr i*64",
    "i<<8", "i<<16", "i<<32", "i<<48", "i<<32|i"};
constexpr int kNumPatterns = sizeof(kPatterns) / sizeof(kPatterns[0]);

// Random hashes give all 254 H2 values, all overflow bits, and about 1005 of
// 1024 groups (sd 4). Stricter than test_hash_bits_structured_keys, so that
// a kept multiplier is close to random, not just above the test's bar.
static bool Spreads(Pattern p, uint64_t t, uint64_t m) {
  const uint64_t n = 4096;
  const size_t ng = 1024;
  bool h2[256] = {}, ovf[64] = {};
  bool grp[ng] = {};
  size_t n_h2 = 0, n_ovf = 0, n_grp = 0;
  for (uint64_t i = 0; i < n; ++i) {
    const uint64_t h = Mix(p(i), t, m);
    const unsigned b = H2(h);
    if (!h2[b]) h2[b] = true, ++n_h2;
    const unsigned o = (h >> 8) & (SFLAT_OVERFLOW_BITS - 1);
    if (!ovf[o]) ovf[o] = true, ++n_ovf;
    const size_t g = Fastrange(h, ng);
    if (!grp[g]) grp[g] = true, ++n_grp;
  }
  return n_h2 >= 250 && n_ovf == SFLAT_OVERFLOW_BITS && n_grp >= 960;
}

// Fraction of (pattern, t) cases that spread badly.
static double QualityFails(uint64_t m, const std::vector<uint64_t>& ts,
                           int* worst_pattern) {
  int fails = 0, per[kNumPatterns] = {};
  for (int p = 0; p < kNumPatterns; ++p)
    for (uint64_t t : ts)
      if (!Spreads(kPatterns[p], t, m)) ++fails, ++per[p];
  if (worst_pattern)
    *worst_pattern = int(std::max_element(per, per + kNumPatterns) - per);
  return double(fails) / (kNumPatterns * ts.size());
}

constexpr double kMaxPairProbe = 10;

// Mean probe length when the keys of table A, in A's iteration order (its
// home order), go into a table B with half as many slots, up to load 0.8,
// with linear probing. Independent orders give about 13; orders that are
// shifts or few interleaved runs of each other give hundreds or more.
static double PairProbe(Pattern p, uint64_t ta, uint64_t ma, uint64_t tb,
                        uint64_t mb) {
  const size_t n = 1 << 15, cap = n / 2, inserts = cap * 4 / 5;
  std::vector<std::pair<uint32_t, uint64_t>> order(n);  // (home in A, key)
  for (size_t i = 0; i < n; ++i) {
    const uint64_t k = p(i);
    order[i] = {uint32_t(Mix(k, ta, ma) >> 32), k};
  }
  std::sort(order.begin(), order.end());
  std::vector<bool> used(cap);
  uint64_t probes = 0;
  for (size_t i = 0; i < inserts; ++i) {
    size_t s = Fastrange(Mix(order[i].second, tb, mb), cap);
    while (used[s]) s = s + 1 == cap ? 0 : s + 1, ++probes;
    used[s] = true;
    ++probes;
  }
  return double(probes) / inserts;
}

static const Pattern kPairPatterns[] = {kPatterns[0], kPatterns[2],
                                        kPatterns[5], kPatterns[9]};

static double WorstPair(uint64_t ta, uint64_t ma, uint64_t tb, uint64_t mb) {
  double w = 0;
  for (Pattern p : kPairPatterns) w = std::max(w, PairProbe(p, ta, ma, tb, mb));
  return w;
}

// Bits that must differ in every window of the given width. 6 bits in 16
// are not reachable for 64 entries; small tables are cheap even when keys
// pile up, so the smaller window gets the weaker bound.
struct Window { int width, min_distance; };
constexpr Window kWindows[] = {{16, 4}, {24, 6}};

static bool FarApart(uint64_t a, uint64_t b) {
  const uint64_t d = a ^ b;
  for (const Window& w : kWindows)
    for (int lo = 0; lo + w.width <= 64; ++lo)
      if (__builtin_popcountll((d >> lo) & ((1ULL << w.width) - 1)) <
          w.min_distance)
        return false;
  return true;
}

static bool AllSpread(uint64_t m, const std::vector<uint64_t>& ts) {
  for (uint64_t t : ts)
    for (Pattern p : kPatterns)
      if (!Spreads(p, t, m)) return false;
  return true;
}

static void Print(const char* name, const std::vector<uint64_t>& v) {
  std::printf("inline constexpr uint64_t %s[%zu] = {\n", name, v.size());
  for (size_t i = 0; i < v.size(); ++i)
    std::printf("%s0x%016llXULL,%s", i % 3 == 0 ? "    " : " ",
                (unsigned long long)v[i], i % 3 == 2 ? "\n" : "");
  std::printf("%s};\n", v.size() % 3 ? "\n" : "");
}

int main(int argc, char** argv) {
  constexpr uint64_t kGolden = 0x9E3779B97F4A7C15ULL;
  std::mt19937_64 rng(20261107);

  if (argc > 1 && std::strcmp(argv[1], "--check") == 0) {
    std::vector<uint64_t> ts(256);
    for (auto& t : ts) t = rng();
    int wp;
    const double q = QualityFails(kGolden, ts, &wp);
    std::printf("golden, random T: quality fails %.2f%% (worst pattern %s)\n",
                q * 100, kPatternNames[wp]);
    const uint64_t s = 0x9E3779B1u;
    std::printf("pair probes, same multiplier:\n");
    std::printf("  random T:           %.1f\n", WorstPair(rng(), kGolden, rng(), kGolden));
    std::printf("  T differ in 1 bit:  %.1f\n", WorstPair(s, kGolden, s ^ 4, kGolden));
    std::printf("  T differ in bit 31: %.1f\n", WorstPair(s, kGolden, s ^ (1u << 31), kGolden));
    std::printf("  same T:             %.1f\n", WorstPair(s, kGolden, s, kGolden));
    return 0;
  }

  const size_t want = argc > 1 ? std::strtoul(argv[1], nullptr, 10) : 64;

  std::vector<uint64_t> ts;
  int t_tried = 0;
  while (ts.size() < want) {
    const uint64_t t = rng();
    ++t_tried;
    bool ok = true;
    for (uint64_t u : ts) ok = ok && FarApart(t, u);
    if (ok) ts.push_back(t);
  }
  std::fprintf(stderr, "T: %zu of %d candidates\n", ts.size(), t_tried);

  // Candidate k is SplitMix64(k) | 1, so the result does not depend on the
  // number of threads: the quality check (almost all of the time) runs in
  // parallel over a batch, and the pair checks run in candidate order.
  const auto candidate = [](uint64_t k) {
    uint64_t z = (k + 1) * 0x9E3779B97F4A7C15ULL;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return (z ^ (z >> 31)) | 1;
  };
  const unsigned threads = std::max(1u, std::thread::hardware_concurrency());
  constexpr uint64_t kBatch = 1 << 14;
  std::vector<uint64_t> ms;
  uint64_t tried = 0, bad_quality = 0, bad_pair = 0;
  double worst_kept = 0;
  if (argc > 2) {  // lines "M: n/count  0x...  tried k  worst pair probe w"
    FILE* f = std::fopen(argv[2], "r");
    if (!f) return std::perror(argv[2]), 1;
    char line[256];
    while (std::fgets(line, sizeof line, f)) {
      unsigned n, c;
      unsigned long long m, k;
      double w;
      if (std::sscanf(line, "M: %u/%u 0x%llx tried %llu worst pair probe %lf",
                      &n, &c, &m, &k, &w) == 5 && n == ms.size() + 1) {
        ms.push_back(m);
        tried = k;
        worst_kept = std::max(worst_kept, w);
      }
    }
    std::fclose(f);
    std::fprintf(stderr, "resumed: %zu kept, %llu tried\n", ms.size(),
                 (unsigned long long)tried);
  }
  std::vector<char> spreads(kBatch);
  for (uint64_t base = tried; ms.size() < want; base += kBatch) {
    std::vector<std::thread> pool;
    for (unsigned w = 0; w < threads; ++w)
      pool.emplace_back([&, w] {
        for (uint64_t k = w; k < kBatch; k += threads) {
          const uint64_t m = candidate(base + k);
          const int pc = __builtin_popcountll(m);
          spreads[k] = pc >= 28 && pc <= 36 && AllSpread(m, ts);
        }
      });
    for (auto& t : pool) t.join();
    for (uint64_t k = 0; k < kBatch && ms.size() < want; ++k) {
      ++tried;
      if (!spreads[k]) { ++bad_quality; continue; }
      const uint64_t m = candidate(base + k);
      std::mt19937_64 pick(m);  // which T entries the pair checks use
      double w = 0;
      for (int i = 0; i < 8 && w <= kMaxPairProbe; ++i) {  // itself, other T
        const size_t a = pick() % ts.size();
        const size_t b = (a + 1 + pick() % (ts.size() - 1)) % ts.size();
        w = std::max(w, WorstPair(ts[a], m, ts[b], m));
      }
      for (size_t i = 0; i < ms.size() && w <= kMaxPairProbe; ++i) {
        const size_t a = pick() % ts.size(), b = pick() % ts.size();
        w = std::max({w, WorstPair(ts[a], ms[i], ts[b], m),
                      WorstPair(ts[b], m, ts[a], ms[i])});
      }
      if (w > kMaxPairProbe) { ++bad_pair; continue; }
      ms.push_back(m);
      worst_kept = std::max(worst_kept, w);
      std::fprintf(stderr, "M: %2zu/%zu  0x%016llX  tried %llu  worst pair probe %.1f\n",
                   ms.size(), want, (unsigned long long)m,
                   (unsigned long long)tried, w);
    }
  }
  std::fprintf(stderr,
               "M: tried %llu: %llu failed quality, %llu failed a pair; worst "
               "kept pair probe %.1f (independent orders: about 3)\n",
               (unsigned long long)tried, (unsigned long long)bad_quality,
               (unsigned long long)bad_pair, worst_kept);
  Print("kSeedMultipliers", ms);
  Print("kSeedXors", ts);
}
