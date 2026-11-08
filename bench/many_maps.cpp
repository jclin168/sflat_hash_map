// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jian-Cheng Lin
// Many small maps, accessed in a scattered order, so that the map object
// itself misses the cache: measures the member layout of the fast paths
// (how many cache lines of the object an insert or a find touches).
//
// 131072 maps of 12 keys (18 MB of map objects alone). Insert fills the
// maps a second time, after clear(), so that the first touch of the memory
// (page faults) is not timed. Prints the best of 5 runs, in ns per call.
//
// The insert loop stores into the map, and the address of that store is known
// only after the lookup, so the cache misses of successive calls do not
// overlap as they do for find: inserts here cost about 4x as much as finds,
// with any map.
//
// Build: g++ -std=c++17 -O3 -march=native -DSFLAT_PROCESS_SEED=1
//        -Iinclude bench/many_maps.cpp
// (a fixed process seed makes runs comparable).
#include <sflat/flat_hash_map.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <random>
#include <vector>

using Map = sflat::flat_hash_map<uint64_t, uint64_t>;

static double Now() {
  return std::chrono::duration<double>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

int main() {
  constexpr size_t kMaps = 1 << 17, kKeys = 12, kOps = 20000000;
  std::mt19937_64 rng(1);
  std::vector<uint32_t> which(kOps);
  std::vector<uint64_t> key(kOps);
  for (size_t i = 0; i < kOps; ++i) {
    which[i] = static_cast<uint32_t>(rng() % kMaps);
    key[i] = rng() % kKeys;
  }
  double best_insert = 1e9, best_hit = 1e9, best_miss = 1e9;
  uint64_t sink = 0;
  for (int rep = 0; rep < 5; ++rep) {
    std::vector<Map> maps(kMaps);
    for (auto& m : maps) m.reserve(kKeys);
    const auto fill = [&] {
      for (size_t k = 0; k < kKeys; ++k)
        for (size_t i = 0; i < kMaps; ++i)
          maps[(i * 0x9E3779B1u) % kMaps][k] = k;
    };
    fill();
    for (auto& m : maps) m.clear();
    double t = Now();
    fill();
    best_insert = std::min(best_insert, (Now() - t) / (kKeys * kMaps));
    t = Now();
    for (size_t i = 0; i < kOps; ++i) sink += maps[which[i]].find(key[i])->second;
    best_hit = std::min(best_hit, (Now() - t) / kOps);
    t = Now();
    for (size_t i = 0; i < kOps; ++i) {
      const Map& m = maps[which[i]];
      sink += m.find(key[i] + kKeys) == m.end();
    }
    best_miss = std::min(best_miss, (Now() - t) / kOps);
  }
  std::printf("%zu maps x %zu keys, sizeof %zu: insert %.1f  find hit %.1f  "
              "find miss %.1f ns  (%u)\n",
              kMaps, kKeys, sizeof(Map), best_insert * 1e9, best_hit * 1e9,
              best_miss * 1e9, static_cast<unsigned>(sink & 1));
}
