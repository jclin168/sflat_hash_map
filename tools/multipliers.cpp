// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jian-Cheng Lin
// Searches the multipliers of the seeded mixer:
//   h = r ^ (r >> 64),  r = x * M[i]   (128-bit product)
// where the seed of a table picks i. The output replaces kSeedMultipliers in
// include/sflat/hash_mix.hpp.
//
// A multiplier is kept if keys with regular bits (sequential IDs, aligned
// pointers, shifted IDs) spread over all H2 values, all overflow bits and
// the groups, like random hashes do. About 1 in 11 candidates passes.
//
// Pairs of multipliers are not checked. The orders of two tables matter
// when one is iterated and its keys inserted into the other, which piles up
// when the two mixers are the same or almost the same (one multiplier with
// seeds xored into the input that differ in one bit: 48-400x slower). For
// two different multipliers it does not: a simulation (keys in the order of
// one, into a half-size table under the other at load 0.8) gives high mean
// probes for about 10% of pairs, up to 1000, but those pairs took 0.9-1.1x
// the usual time in the map (2M keys; the map grows and migrates, which the
// simulation does not). A table that does pile up, as with the same
// multiplier, takes a new seed (see the safety net in flat_hash_map.hpp).
//
// No xor or other input transform: with one multiply, tables with the same
// multiplier have related orders whatever else differs, and different
// multipliers did as well without it.
//
// Candidate k is SplitMix64(k) | 1, checked on all threads but kept in
// candidate order, so the result does not depend on the number of threads.
//
// Build: g++ -std=c++17 -O2 -march=native -pthread -Iinclude tools/multipliers.cpp
// Run:   ./a.out [count]   prints the table as C++
#include <sflat/flat_hash_map.hpp>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

using sflat::detail::Fastrange;
using sflat::detail::H2;

static uint64_t Mix(uint64_t x, uint64_t m) {
  const __uint128_t r = (__uint128_t)x * m;
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

// Random hashes give all 254 H2 values, all overflow bits, and about 1005 of
// 1024 groups (sd 4). Stricter than test_hash_bits_structured_keys, so that
// a kept multiplier is close to random, not just above the test's bar.
static bool Spreads(Pattern p, uint64_t m) {
  const uint64_t n = 4096;
  const size_t ng = 1024;
  bool h2[256] = {}, ovf[64] = {};
  bool grp[ng] = {};
  size_t n_h2 = 0, n_ovf = 0, n_grp = 0;
  for (uint64_t i = 0; i < n; ++i) {
    const uint64_t h = Mix(p(i), m);
    const unsigned b = H2(h);
    if (!h2[b]) h2[b] = true, ++n_h2;
    const unsigned o = (h >> 8) & (SFLAT_OVERFLOW_BITS - 1);
    if (!ovf[o]) ovf[o] = true, ++n_ovf;
    const size_t g = Fastrange(h, ng);
    if (!grp[g]) grp[g] = true, ++n_grp;
  }
  return n_h2 >= 250 && n_ovf == SFLAT_OVERFLOW_BITS && n_grp >= 960;
}

static bool AllSpread(uint64_t m) {
  const int pc = __builtin_popcountll(m);
  if (pc < 28 || pc > 36) return false;
  for (Pattern p : kPatterns)
    if (!Spreads(p, m)) return false;
  return true;
}

static void Print(const char* name, const std::vector<uint64_t>& v) {
  std::printf("inline constexpr uint64_t %s[%zu] = {\n", name, v.size());
  for (size_t i = 0; i < v.size(); ++i)
    std::printf("%s0x%016llXULL,%s", i % 3 == 0 ? "    " : " ",
                (unsigned long long)v[i], i % 3 == 2 ? "\n" : "");
  std::printf("%s};\n", v.size() % 3 ? "\n" : "");
}

static uint64_t Candidate(uint64_t k) {
  uint64_t z = (k + 1) * 0x9E3779B97F4A7C15ULL;
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
  return (z ^ (z >> 31)) | 1;
}

// Runs f(i) for i in [0, n) on all threads.
template <typename F>
static void Parallel(size_t n, F f) {
  const unsigned threads = std::max(1u, std::thread::hardware_concurrency());
  std::atomic<size_t> next{0};
  std::vector<std::thread> pool;
  for (unsigned t = 0; t < threads; ++t)
    pool.emplace_back([&] {
      for (size_t i; (i = next.fetch_add(1)) < n;) f(i);
    });
  for (auto& t : pool) t.join();
}

int main(int argc, char** argv) {
  const size_t want = argc > 1 ? std::strtoul(argv[1], nullptr, 10) : 4096;
  constexpr size_t kBatch = 4096;
  std::vector<uint64_t> ms;
  std::vector<char> spreads(kBatch);
  uint64_t tried = 0;
  while (ms.size() < want) {
    const uint64_t base = tried;
    Parallel(kBatch, [&](size_t k) { spreads[k] = AllSpread(Candidate(base + k)); });
    for (size_t k = 0; k < kBatch && ms.size() < want; ++k) {
      ++tried;
      if (spreads[k]) ms.push_back(Candidate(base + k));
    }
  }
  std::fprintf(stderr, "kept %zu of %llu candidates\n", ms.size(),
               (unsigned long long)tried);
  Print("kSeedMultipliers", ms);
}
