# sflat::flat_hash_map

sflat::flat_hash_map is a C++17 header-only hash map. It uses open
addressing. The interface matches the common subset of std::unordered_map.

The design has three goals. First, insert and find are fast. Second, the
growth factor gets smaller as the table gets large (2x, 1.5x, 1.25x). This
uses less memory for large tables. Third, growth is
incremental and gives old pages back to the kernel. This avoids a latency
spike and keeps the growth memory peak low. The map suits large data sets
where memory is important.

## Why this project

- `std::unordered_map` is too slow.
- Other flat hash maps use too much memory. They grow 2x, so a large table
  goes from 32 GB to 64 GB to 128 GB or more. This is not acceptable.
- The usual use case is simple: you know the number of entries, you fill
  the map, and then you do fast lookups. Iteration is almost never
  necessary. Lookups of missing keys are rare.
- Transparent huge pages (THP) can make lookups in large tables faster.

These requirements are the reasons for this project.

## File structure

```
include/sflat/flat_hash_map.hpp   # the only header you need
tests/correctness.cpp             # correctness tests
bench/bench.cpp                   # benchmark program
BENCHMARK.md                      # benchmark results for all machines
thirdparty/emhash/                # emhash (for comparison only, not in git)
thirdparty/boost/                 # boost headers (optional, else system boost)
```

## Quick start

```cpp
#include <sflat/flat_hash_map.hpp>

sflat::flat_hash_map<uint64_t, uint64_t> m;
m.reserve(1000000);
m.emplace(42, 100);
auto it = m.find(42);  // it->second == 100
```

Compile with this command:

`g++ -std=c++17 -O3 -march=native -I include your_file.cpp`

On ARM (for example Apple Silicon) NEON is part of the base AArch64 ISA, so
no flag is needed. `-mcpu=native` is fine too:

`clang++ -std=c++17 -O3 -mcpu=native -I include your_file.cpp`

## Design

### Memory layout

The map has three arrays.

- **slots**: a dense array of `pair<const Key, T>`. Each slot is 16 bytes
  for `uint64_t -> uint64_t`.
- **ctrl**: a control array with one byte per slot. The style follows
  SwissTable. `0xFF` means empty. `0xFE` means deleted. Values in
  `[0, 0xFD]` hold the 8-bit hash fingerprint (H2).
- **overflow**: one 32-bit word per group (32 slots with AVX2, 16 with
  SSE2 and NEON). Each word is a Bloom filter. See below.

### Capacity and growth

- The capacity is always a multiple of the SIMD group width. The width is
  32 with AVX2, 16 with SSE2 and NEON, and 8 with the portable fallback.
  The capacity does not need to be a power of two.
- The map uses 32-bit Lemire fastrange for positioning:
  `(n * (h >> 32)) >> 32`. This is one `IMUL r32` instruction (1 uop). It
  is about 5 times faster than 128-bit multiplication. It does not need a
  power of two, unlike a bitmask.
- The growth factor depends on the size of the current table (slots plus
  control bytes):

  | Current table size | Growth factor |
  |--------------------|---------------|
  | less than 2 GiB    | 2x            |
  | 2 GiB to 10 GiB    | 1.5x          |
  | 10 GiB or more     | 1.25x         |

  Small tables grow 2x, so each element moves fewer times. Large tables
  grow slowly, so the unused capacity and the growth peak stay small. For
  `uint64_t -> uint64_t` (17 bytes per slot), 2 GiB is about 126M slots
  (110M entries) and 10 GiB is about 630M slots (550M entries). Set the
  limits in bytes with `SFLAT_GROW_2X_BELOW` and `SFLAT_GROW_1_5X_BELOW`.
- The default max load factor is 0.875.
- **Incremental rehash**: the map does not move all items at once during
  growth. Each insert moves the next 64 old slots (two groups) to the new
  table. This avoids a latency spike. See "Growth memory" below for how the
  map keeps the memory peak low.
- During a migration, `find` checks the new table first and then the old
  table. `find`, `erase` and iteration never move items. Only inserts move
  the migration forward. `finish_migration()` completes it at once.

Memory formula for `uint64_t -> uint64_t`:
```
bytes/entry = (16 + 1 + 4/16) / 0.875 = about 19.7   (SSE2, NEON)
bytes/entry = (16 + 1 + 4/32) / 0.875 = about 19.6   (AVX2)
```
The 4 is the 32-bit overflow word of each group. The measured value is
19.7 bytes/entry at 100M on the i5-3230M (19.5 with the older 8-bit
overflow byte). Small tables measure up to about 22 bytes/entry; the
difference is allocator overhead.

### Growth memory (Linux)

With `std::allocator` on Linux, each array of 2 MiB or more gets its own
2 MiB-aligned `mmap`. Smaller arrays use `operator new` with 64-byte
alignment, so a control group never crosses two cache lines.

A plain incremental rehash keeps the full old table (1x) and the full new
table (1.25x to 2x) alive until the migration ends. The map avoids this peak in
three steps:

1. **Ordered migration.** Fastrange is monotonic in the hash. A key at
   relative position x in the old table has its home at the same relative
   position x in the new table. The migration reads the old table in index
   order, so it writes the new table in index order too.
2. **New keys stay in order.** During a migration, a new key whose old home
   group is ahead of the migration front goes into the old table. The old
   table still has about 12 percent free slots. The migration moves the key
   later. Thus the new table gets writes only near the front.
3. **Page release.** Old slots behind the front are dead. The map gives
   them back to the kernel with `madvise(MADV_DONTNEED)`, one 2 MiB page at
   a time. New pages are committed only when the front reaches them.

At progress f, the resident memory is about `old * (1 - f) + new * f`. The
peak is near the size of the new table, not old + new. The control bytes of
both tables (1 byte per slot) stay resident until the migration ends.

`reserve()` and `rehash()` do a full rehash at once. They also move items in
index order and release the old slots behind the cursor.

### Transparent huge pages (THP)

Large tables cause many TLB misses with 4 KiB pages. The map asks for huge
pages with `madvise(MADV_HUGEPAGE)`:

- control bytes, overflow bytes, and slots after `reserve()`: at once;
- slots of a growing table: a 2 MiB page just ahead of the migration front.
  Pages far ahead of the front stay `MADV_NOHUGEPAGE`, so a rare stray write
  does not commit a full 2 MiB page;
- at the end of a migration, `MADV_COLLAPSE` (Linux 6.1 and later) converts
  the few 4 KiB pages that were written before their hint.

The system THP setting must be `madvise` or `always`
(`/sys/kernel/mm/transparent_hugepage/enabled`). With `never`, the hints
have no effect.

Configuration macros (define before the include):

| Macro | Default | Effect |
|-------|---------|--------|
| `SFLAT_USE_MMAP` | 1 on Linux | 0: use `operator new` for all arrays. No page release, no THP. |
| `SFLAT_USE_THP` | 1 | 0: keep `mmap` and page release, but give no huge page hints. |
| `SFLAT_GROW_2X_BELOW` | 2 GiB | Tables smaller than this many bytes grow 2x. |
| `SFLAT_GROW_1_5X_BELOW` | 10 GiB | Tables smaller than this grow 1.5x; larger tables grow 1.25x. |
| `SFLAT_NO_SIMD` | 0 | 1: use the portable SWAR group (8 bytes) instead of AVX2, SSE2 or NEON. |
| `SFLAT_PREFETCH_SLOTS` | 1 with NEON, else 0 | 1: `find()` prefetches the home group's slots before it reads the control bytes. |
| `SFLAT_OVERFLOW_BITS` | 32 | Bits per group in the overflow Bloom filter: 8, 16, 32 or 64. See "Overflow word". |

A custom allocator disables both. The map then uses only the allocator.

On macOS and other non-Linux systems `SFLAT_USE_MMAP` defaults to 0: all
arrays come from `operator new`, so there is no page release and no THP.
Incremental growth still works (no latency spike), but the growth peak is
old table + new table.

### Why a smaller growth factor uses less memory

A 2x growth needs a 3x peak during rehash (old table plus new table). The
steady-state capacity can reach 2x of the need. A 1.5x growth reduces these
to about 2.5x and 1.5x. For 100M entries at 16 bytes each:

| Strategy | Steady-state capacity | Rehash peak |
|----------|----------------------|-------------|
| 2x       | about 3.2 GB         | about 4.8 GB |
| 1.5x     | about 2.1 GB         | about 3.5 GB |

These are theoretical estimates for a plain rehash. They are the worst case
for each strategy. sflat releases old pages during growth, so its measured
peak is lower (see "Growth memory"). With 1.25x growth the steady-state
capacity is at most 1.25x of the need.

### SIMD probing

The back end is chosen at compile time:

| Back end | When | Group | Match |
|----------|------|-------|-------|
| AVX2 | x86-64 with `__AVX2__` | 32 bytes | `vpcmpeqb` + `vpmovmskb`, 1 bit per lane |
| SSE2 | other x86-64 | 16 bytes | `pcmpeqb` + `pmovmskb`, 1 bit per lane |
| NEON | ARM with NEON (AArch64, Apple Silicon, ARMv7 + NEON), little endian | 16 bytes | `cmeq` + `shrn #4` + `fmov`, 4 bits per lane |
| SWAR | any other CPU, or `SFLAT_NO_SIMD=1` | 8 bytes | exact zero-byte test on a `uint64_t`, 8 bits per lane |

- NEON has no `movemask` instruction. The map narrows the 16-byte compare
  result with `shrn` (shift right by 4 and narrow each 16-bit pair) to a
  64-bit value with 4 bits per lane, and moves it to a general register.
  Masking with `0x1111...` keeps one bit per lane, so the usual
  `m &= m - 1` loop works and the lane index is `ctz(m) / 4`. As `ctz(m)`
  is a multiple of 4, the slot offset is one shift. With
  `-O3` a match is `dup`, `cmeq`, `shrn`, `fmov`, `and` (the `dup` is
  hoisted out of loops for the empty and deleted values).
- The probe loops use `Group::Mask`, `Group::Lane()` and `Group::kAll`, so
  the code is the same for all back ends.
- Probing is linear by group.

### Hash strategy

The map processes the hash in three steps.

1. It takes `std::hash<Key>`. You can supply your own hash.
2. It applies a boost-style mulx64 avalanche: `(x * C) ^ ((x * C) >> 64)`.
   This is one 128-bit multiplication. It spreads weak hashes well, for
   example the identity hash of sequential integers.
3. It uses the high 32 bits of the mixed hash for fastrange positioning.
   It uses the low 8 bits for H2. It uses bits [8, 13) for the overflow bit
   (bits [8, 8 + log2(SFLAT_OVERFLOW_BITS)) in general).
   The three parts do not overlap. This reduces correlation.

### Overflow word (fast miss)

Each group has a 32-bit Bloom filter. When a key overflows from its home
group to a later group because the home group is full, the map sets one bit
(chosen by the hash) in the home group's word. When `find` does not locate
the key in the home group, a miss ends there if the home group has an empty
slot or the key's bit is clear. Only otherwise does `find` probe the next
groups. The idea comes from `boost::unordered_flat_map`, which keeps an
8-bit filter inside each group.

Why 32 bits: at the 0.875 load factor about 48 percent of the home groups
are full. With an 8-bit filter, about 20 percent of the misses in a full
group find their bit set by chance, so 9.5 percent of all misses probed a
second group. Such a miss is expensive: its branch waits for a DRAM load,
so a mispredict costs about one memory latency (measured on the i5-3230M:
about 140 ns per probing miss, half of the total miss time). With 32 bits
the share drops to 2.7 percent (64 bits: 1.4 percent). Measured on the
i5-3230M, misses got 12 to 20 percent faster, hits did not change, and the
cost is 0.19 bytes per slot (about 1 percent). A filter inside the control
cache line (as in boost) was also tried: it saved no time, because the
extra address arithmetic slowed down hits (see BENCHMARK.md, "Profiling
on the i5-3230M").

`find` reads the overflow word together with the control bytes, not only
after they show a full group. The two loads then overlap, and the test
"empty slot or clear bit" needs no hard-to-predict branch.

`erase` does not clear overflow bits. This is conservative and correct.
Rehash rebuilds the bits.

## API compatibility

The map provides the common `std::unordered_map` interface. This includes
constructors, copy and move, swap, `insert`, `emplace`, `try_emplace`,
`insert_or_assign`, `operator[]`, `at`, `find`, `count`, `contains`,
`erase`, `clear`, iterators, `reserve`, `rehash`, `load_factor`,
`max_load_factor`, `bucket_count`, `bucket_size`, `hash_function`,
`key_eq`, `get_allocator`, and comparison operators.

Limits (common trade-offs of open addressing):

- Iterators are not node-based. If `insert` triggers a rehash, all
  iterators become invalid. `std::unordered_map` guarantees that `insert`
  does not invalidate iterators. This map does not give that guarantee.
- Bucket and local iterators do not have full standard semantics.
  `begin(size_t)` and related functions are approximate.
- `erase` uses tombstones. After many deletions, call `rehash`.
- `const` functions never change the map, also during a migration. Many
  threads can read at the same time while no thread writes.
- `erase` never moves other items. `it = m.erase(it)` loops are safe, also
  during a migration.
- Capacity limit: fewer than 2^32 groups, because the position function
  (Fastrange) is 32-bit. With AVX2 (32 slots per group) this is about 137
  billion slots, or 120 billion entries at the 0.875 load factor. With SSE2
  and NEON it is half, and with SWAR a quarter. `max_size()` returns the
  limit. Above it, `insert`, `reserve` and `rehash` throw
  `std::length_error`.

### Steady-state optimization

When the map will have no more inserts or erases, call `optimize()`. It
rebuilds the table, removes tombstones, and shrinks to the minimal capacity.
This shortens probe chains and speeds up `find`. `shrink_to_fit()` does the
same resize without the semantic hint.

The gain is modest (about 5 percent with 30 percent tombstones). At large
scales the bottleneck is cache misses from table size, not probe length.
No reorganization can fix that.

### Batch find

`find_batch(keys, n, out_found, out_values)` looks up `n` keys at once. It
prefetches home buckets before the finds to overlap DRAM accesses. This
helps when the table does not fit in cache.

Measured speedup (older version of the map): 1.04x at 100M entries (80.8 ns
to 77.9 ns per op). No gain at 10M. The CPU already overlaps misses well in a simple loop. The
benefit grows with table size.

## Benchmark

The full results are in [BENCHMARK.md](BENCHMARK.md). It covers four
machines: an Intel Xeon KVM virtual machine (AVX2), an Intel Celeron N5105
and an Intel Core i5-3230M on bare metal (SSE2), and an Apple M2 (NEON). It
also has the build and run steps.

In short (`uint64_t -> uint64_t`, random keys, compared with emhash8,
boost::unordered_flat_map, absl::flat_hash_map and the node-based maps):

- **Memory**: sflat uses the least memory in steady state on every machine
  (19.5 to 21.8 bytes/entry). On Linux at 50M to 100M entries it also has
  the lowest growth peak.
- **Find hit (random order)**: on Linux x86-64, sflat is the fastest or
  within 20 percent of the fastest at every size. On the M2 (no THP)
  absl_flat and boost_flat are faster at 10M and 100M.
- **Insert**: on the Celeron and the i5, sflat is the fastest in most
  rows. On the Xeon VM, boost_flat and absl_flat are faster at 1M and 10M
  (huge page faults are slow in the VM). On the M2, boost_flat is the
  fastest.
- On the machines with small caches and TLBs (Celeron, i5) much of the lead
  of sflat comes from transparent huge pages, not from the probing scheme.
- **Find miss**: boost_flat is faster on every machine (on the i5-3230M,
  with the current code, by 1.1 to 1.3 times).
- **Iterate**: not a goal; emhash8 is much faster.

## Correctness tests

Run these commands:

```bash
g++ -std=c++17 -O2 -march=native -I include tests/correctness.cpp -o /tmp/correctness
/tmp/correctness
g++ -std=c++17 -O2 -march=native -I include tests/growth_tiers.cpp -o /tmp/growth_tiers
/tmp/growth_tiers
```

The tests include: randomized mixed operations against
`std::unordered_map`, string keys, sequential keys, copy and move, swap,
iterator erase, reserve and rehash, operations during a migration, large
tables on the mmap and THP path, the capacity limit, and growth factor
checks. `growth_tiers.cpp` sets small tier limits and checks the 2x, 1.5x
and 1.25x steps. `test_group_match` checks the SIMD back end against a
scalar reference for every lane and every byte value.

All tests pass on each back end:

| Back end | How it was tested |
|----------|-------------------|
| AVX2 | x86-64 Linux, `-march=native`; also `clang++ -arch x86_64 -mavx2` under Rosetta 2 on Apple M2 |
| SSE2 | `-mno-avx2` on x86-64 Linux; `clang++ -arch x86_64` under Rosetta 2 |
| NEON | Apple M2, macOS 26.6, Apple clang 21 |
| SWAR | Apple M2 with `-DSFLAT_NO_SIMD=1` |

NEON and SWAR also pass with ASan and UBSan on the M2. (The x86 paths were
checked with ASan and UBSan on Linux before the NEON change; this change was
not re-run there.)

## License

This project uses the Apache License 2.0. The LICENSE file has the full
text. Each source file has an `SPDX-License-Identifier: Apache-2.0` line.

The license covers `include/`, `tests/`, and `bench/`. `thirdparty/` holds
third-party code. Each third-party item keeps its original license (emhash,
Boost).
