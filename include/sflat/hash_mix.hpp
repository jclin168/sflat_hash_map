// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jian-Cheng Lin
#pragma once
//
// Hash mixing and per-table seeds of sflat::flat_hash_map and
// sflat::flat_hash_set: the mixer applied to the user hash, the seed each
// table takes at its first allocation, and the checked mixers a seed picks.
//
// Configuration macros:
//   SFLAT_NO_SEED=1       every table uses MixHash without a seed: the same
//                         iteration order for the same keys in every table
//                         and every run, as before seeds. For debugging. A
//                         loop that inserts while iterating another table
//                         without reserve() can then take quadratic time.
//   SFLAT_PROCESS_SEED=n  seeds start at n instead of a random value, so a
//                         run repeats (iteration order, benchmarks). Tables
//                         still get different seeds.

#include <atomic>
#include <chrono>
#include <cstdint>

#if !defined(SFLAT_NO_SEED)
#define SFLAT_NO_SEED 0
#endif

#if !defined(SFLAT_PROCESS_SEED)
#if defined(__linux__) && defined(__has_include)
#if __has_include(<sys/random.h>)
#include <sys/random.h>
#define SFLAT_HAS_GETRANDOM 1
#endif
#endif
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || \
    defined(__NetBSD__)
#include <stdlib.h>  // arc4random
#define SFLAT_HAS_ARC4RANDOM 1
#endif
#endif

namespace sflat {
namespace detail {

// Single-multiply avalanche mixer, boost-style mulx64:
//   (x * C) ^ ((x * C) >> 64),  C = 2^64/phi (golden ratio).
// One 128-bit multiply instead of a splitmix64 finalizer (which needs two
// *dependent* 64-bit multiplies): much shorter critical path, while still
// robust against weak hashes such as std::hash<uint64_t> identity on
// sequential keys.
//
// Do not drop the high half (a plain 64-bit x * C). In a 64-bit product,
// bit k depends only on bits [0, k] of x. Then H2 (bits [0,8)) depends only
// on the low 8 bits of the key, and the overflow bit only on the low 13
// bits. Keys with regular low bits (aligned pointers, i << 8, i * 64) then
// share a few H2 values or one, and a find compares many keys: up to 10x
// slower. The xor of the high half makes every output bit depend on every
// key bit.
inline uint64_t MixHash(uint64_t x) noexcept {
  const __uint128_t r = (__uint128_t)x * 0x9E3779B97F4A7C15ULL;
  return (uint64_t)r ^ (uint64_t)(r >> 64);
}

// Seed of a map or set, taken at its first allocation (see SeededMixHash).
// Iteration visits a table in hash order, and Fastrange is monotonic. So
// without a seed, inserting while iterating another table sends the keys in
// sorted home order into a smaller table: they pile up at its start, and a
// loop of m2[kv.first] = kv.second without reserve() takes quadratic time.
//
// The seeds of a process start at a random value, so iteration order also
// changes from run to run and code that depends on it fails early.
inline uint32_t ProcessSeed() noexcept {
#if defined(SFLAT_PROCESS_SEED)
  return static_cast<uint32_t>(SFLAT_PROCESS_SEED);
#else
  uint32_t s = 0;
#if defined(SFLAT_HAS_GETRANDOM)
  if (getrandom(&s, sizeof s, GRND_NONBLOCK) != static_cast<ssize_t>(sizeof s))
    s = 0;
#elif defined(SFLAT_HAS_ARC4RANDOM)
  s = arc4random();
#endif
  if (s == 0) {  // no system random source: a stack address (ASLR) and the time
    const uint64_t t = static_cast<uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    s = static_cast<uint32_t>(MixHash(reinterpret_cast<uintptr_t>(&s) ^ t));
  }
  return s;
#endif
}

// Never 0: 0 means "not taken yet". The step is odd, so the low 12 bits, which
// pick the mixer (see SeededMixHash), differ for any 4096 seeds in a row.
inline uint32_t NextSeed() noexcept {
  static const uint32_t process_seed = ProcessSeed();  // thread-safe init
  static std::atomic<uint32_t> counter{0};
  const uint32_t s =
      process_seed +
      (counter.fetch_add(1, std::memory_order_relaxed) + 1) * 0x9E3779B1U;
  return s ? s : 1U << 12;  // same mixer as 0
}

// Mixers for seeds: SeededMixHash(x, s) is MixHash with kSeedXors[j] xored
// into x and kSeedMultipliers[i] as the multiplier, i = s & 63 and
// j = (s >> 6) & 63, so 4096 mixers. Generated and checked by
// tools/multipliers.cpp; see there for the exact checks.
//
// Xoring a seed into the input of one fixed multiplier does not work. The
// multiply is linear, so the inputs of a key in tables A and B differ by one
// of 2^p values, p = bits of seed A ^ seed B inside the bits that vary
// between keys. Home positions in B are then those in A plus one of 2^p
// offsets, and iterating A feeds B 2^p sorted runs: 1 in 100 random seed
// pairs was 15x slower, and seeds that differ in one bit were quadratic
// again. The xor also spread keys with regular bits badly for some seeds
// (i * 1000: 426 of 1024 groups for 2.4% of seeds). And a second multiply
// would cost 10-40% in find.
//
// So every multiplier spreads 12 patterns of regular keys (sequential IDs,
// aligned pointers, shifted IDs) with every xor like random hashes do (all
// H2 values, all overflow bits, at least 960 of 1024 groups); two
// multipliers scale home positions differently, which breaks the runs, and
// no two of them pile up in a simulation of the loop above; and any two xors
// differ in at least 4 bits in every 16-bit window and 6 in every 24-bit
// window, for two tables with the same multiplier. Two tables with the same
// mixer (seeds 4096 apart in the counter) still give the slow loop.
inline constexpr uint64_t kSeedMultipliers[64] = {
    0x0D4B319EDA031F8BULL, 0x65A2B787AA30CCFDULL, 0x0C66DA0B973385E7ULL,
    0x72C9740CC5A0CFFBULL, 0x25BB4294913885FFULL, 0x9A5198F40558F107ULL,
    0x590190CC63B91FADULL, 0x9D9AB7D183B2C17BULL, 0x16FCED07D998AEB1ULL,
    0x65B141FA2DDACE9FULL, 0xFA5E1A7EA2270311ULL, 0x65D45153836D60B3ULL,
    0x451CA3920A8E16E7ULL, 0x912EA2F35EA5747DULL, 0xBF4CF94E0CE38AC1ULL,
    0x564262DD7B589577ULL, 0x651CC09E55967BF1ULL, 0x947901728F21569BULL,
    0xCE15D37519DB42E5ULL, 0x349CE4E54F4C5A15ULL, 0xCEA37C6477B16781ULL,
    0x26F599E16E9CC8BFULL, 0xBCA94451E78CB221ULL, 0x1CB50FDF81B4A695ULL,
    0xC034C560BC77870FULL, 0x85CA85EBE518BFF1ULL, 0x367464262DBC0BBBULL,
    0xD29B6099795D417BULL, 0xC1C6B85DB2CF76B3ULL, 0x9E192C680DB628ABULL,
    0x21947A54961D153FULL, 0xCADCF0356F221FD7ULL, 0x3C7591869B629F3BULL,
    0x05BC5A69B95864DFULL, 0x5E919B8A5B9FB0E3ULL, 0xA8DC81467AE2724BULL,
    0xC9FA356C58BDCD0FULL, 0xA5E654E642EA46F5ULL, 0x32874A5486B5796FULL,
    0xC3152AA2191BE9B7ULL, 0x03A8BC51CA71CCE1ULL, 0xB392DFDC3B2D5099ULL,
    0x939A509CF523C397ULL, 0x024596E1BEAC57B3ULL, 0xC3715ACAFA5E2C31ULL,
    0x0A5D322949AAE8F9ULL, 0x1AE8DDE3C353277DULL, 0x7D2C9C056D71EA53ULL,
    0x3E691761E606D86FULL, 0xDF0D34058DADC68DULL, 0x8CB88CAD3E4F6605ULL,
    0x53B0B2B93B72DD57ULL, 0x2928C6EF32266467ULL, 0x2CBC32B8A4D5B3A5ULL,
    0x09CCF5B723315925ULL, 0x3458AE40C47D7703ULL, 0x0A292902944F43FFULL,
    0xF767085892D7164DULL, 0x94F3A64B258715A5ULL, 0xBAA117748CBE3D67ULL,
    0xAAF6E325C059B96DULL, 0x59AC5E33D1D6F88FULL, 0x27AA9C1719BCA2C9ULL,
    0xE5833D50B51C16BBULL,
};
inline constexpr uint64_t kSeedXors[64] = {
    0xED6EB3DB8BE76588ULL, 0xC95510B93042F199ULL, 0x931EA9461CDEEAFDULL,
    0x3F8AECCF1008C172ULL, 0xAD436419F4708C25ULL, 0xB7280E9800B790AAULL,
    0x4981B0AEA38B673FULL, 0x6609C338C1110CC5ULL, 0x1E5A8736AC162C2FULL,
    0x6D1DD101570BA895ULL, 0x41EAF686FD2DC7DAULL, 0xD324741280725E6FULL,
    0x80F64605941FA278ULL, 0x7FF15269C9653D28ULL, 0x4EEDCA8B47EACA62ULL,
    0x8733ED72B9F2105CULL, 0x77C2BE0E2AB9AC52ULL, 0x509795D1FE1562D5ULL,
    0x95C077AF9943DE9EULL, 0xAC29674ACDDB53C0ULL, 0xF43738922DA03E25ULL,
    0xE25878B8D2A3D30DULL, 0x43D6EEA0DF86C83DULL, 0x1273D49C5FB16EF3ULL,
    0x31F725ABEEBE20BAULL, 0x00398BA43779B011ULL, 0x76FA71D094F5037FULL,
    0x0D1E0586A66F5D11ULL, 0x0C804FF3C31C7535ULL, 0x454C86402AD6394CULL,
    0x606522C93235DB33ULL, 0xB35B0CA1B4ACA961ULL, 0x2812557D485DC2A6ULL,
    0x4507CEFD2558931AULL, 0x233129C5C1FE0B74ULL, 0x309A02E37663349DULL,
    0xD2827ADE96D37468ULL, 0x02CD2F2279959983ULL, 0x46634DD6EEC3BBF9ULL,
    0x6A7DA7E6E1C5E706ULL, 0x59CB81486182D42EULL, 0x05BC7F9B1E4186E2ULL,
    0x0DB25B45BA7A755EULL, 0xB23AE13B23BCD49BULL, 0x21CF62324BD0072CULL,
    0x966CF5B14A32A1A4ULL, 0xBCF9BC330C2A4684ULL, 0x95633B7A64454C0AULL,
    0xEEB74809B93C4DE0ULL, 0xEBAFB467D7209955ULL, 0x67455C55DFEC9BCFULL,
    0x424EDB9E332E1466ULL, 0x914A2A009A343023ULL, 0xD793275112CD7FFFULL,
    0x58C0E05EE716B742ULL, 0x77A51B2EC89C7A9DULL, 0xFD0B181C8989A5C6ULL,
    0x2E6630683E1C9657ULL, 0xB994085275339E28ULL, 0x9E0F56E61F1A48A7ULL,
    0x649BC82518E4EDFAULL, 0x87509DDA280F7085ULL, 0xE132B7608CAF850EULL,
    0xCC96F21C606B1B21ULL,
};

inline uint64_t SeededMixHash(uint64_t x, uint32_t seed) noexcept {
#if SFLAT_NO_SEED
  (void)seed;
  return MixHash(x);
#else
  const __uint128_t r = (__uint128_t)(x ^ kSeedXors[(seed >> 6) & 63]) *
                        kSeedMultipliers[seed & 63];
  return (uint64_t)r ^ (uint64_t)(r >> 64);
#endif
}

// Mixed hash of key k in a table with the given seed.
template <typename Hash, typename Key>
inline uint64_t HashKey(const Hash& hash, const Key& k, uint32_t seed) {
  return SeededMixHash(static_cast<uint64_t>(hash(k)), seed);
}

}  // namespace detail
}  // namespace sflat
