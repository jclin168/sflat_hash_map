# sflat::flat_hash_map

sflat::flat_hash_map is a C++17 header-only hash map for very large tables.
It uses open addressing with SIMD group probing (AVX2, SSE2, NEON, or a
portable SWAR fallback). The interface matches the common subset of
`std::unordered_map`.

The map has three goals:

- **Fast insert and find.** SIMD compares a group of 16 or 32 control
  bytes at once, and the hot paths are small and inlined.
- **Memory in proportion to the entries.** The capacity is not a power of
  two, and the growth factor gets smaller as the table gets large (2x, 1.5x,
  1.25x).
- **Low memory pressure on Linux.** Growth is incremental and gives old
  pages back to the kernel with `madvise`. Large tables use transparent huge
  pages (THP).

## Why this project

- `std::unordered_map` is too slow.
- Other flat hash maps use too much memory. They grow 2x, so a large table
  goes from 32 GB to 64 GB to 128 GB or more. This is not acceptable.
- The usual use case is simple: you know the number of entries, you fill
  the map, and then you do fast lookups. Iteration is almost never
  necessary. Lookups of missing keys are rare.
- Transparent huge pages (THP) can make lookups in large tables faster.

These requirements are the reasons for this project.

## Key advantages

### Memory grows in proportion to the entries

boost::unordered_flat_map, absl::flat_hash_map, emhash8 and
google::dense_hash_map use a power-of-two capacity. Their table size can
only be 8, 16, 32, 64 or 128 GiB. When the number of entries is a little
more than the limit of one step, the table goes to the next step: twice the
memory.

sflat does not use a power-of-two capacity. `reserve(n)` gives
`ceil(n / 0.875)` slots, rounded up to a multiple of the group width. Thus
the memory per entry stays almost the same at all sizes: for
`uint64_t -> uint64_t`, 21.5 bytes at 1M entries and 19.6 bytes at 100M.

Table size and the maximum number of entries that fit
(`uint64_t -> uint64_t`, after `reserve`):

| Table size | boost_flat | absl_flat | dense_hash_map | sflat for the same entries |
|---|---|---|---|---|
| about 16 GiB | 0.88 billion | 0.94 billion (17 GiB) | 0.54 billion | 16.0 GiB for 0.88 billion |
| about 32 GiB | 1.76 billion | 1.88 billion (34 GiB) | 1.07 billion | 32.0 GiB for 1.76 billion |
| about 64 GiB | 3.52 billion | 3.76 billion (68 GiB) | 2.15 billion | 63.9 GiB for 3.52 billion |
| about 128 GiB | 7.05 billion | 7.52 billion (136 GiB) | 4.29 billion | 127.1 GiB for 7.0 billion |

Example: 1 billion entries need 32 GiB with boost_flat, 34 GiB with
absl_flat and 32 GiB with dense_hash_map. sflat needs about 18.2 GiB.
After the inserts the whole table is resident (random keys write to almost
every page), so this difference is real RSS, not only address space.

Without `reserve` the table grows by steps that get smaller as the table
gets large (see "Capacity and growth"). Thus the unused capacity of a table
of 10 GiB or more is at most 25 percent, not up to 100 percent as with 2x
growth.

### Less memory pressure: huge pages and page release

sflat manages its own pages on Linux. This is the main difference from the
other maps, which use `malloc` with 4 KiB pages and keep the old and new
tables in memory together during a rehash. sflat uses `mmap` and `madvise`
in four ways:

- **Page release during growth (`MADV_DONTNEED`).** The map gives the old
  pages behind the migration front back to the kernel. Thus the growth peak
  is near the size of the new table, not old + new. At 100M entries the
  measured growth peak is 23.8 bytes/entry. boost_flat needs 32.2,
  absl_flat 34.2 and dense_hash_map 64.4.
- **Huge pages (`MADV_HUGEPAGE`).** Large tables use 2 MiB pages. This cuts
  TLB misses. At 100M entries on the Xeon, THP makes find hit 18 percent
  faster and reserved insert 45 percent faster.
- **No early commit (`MADV_NOHUGEPAGE` ahead of the front).** During
  growth, only the pages near the migration front get the huge page hint.
  A rare write far ahead does not commit a full 2 MiB page.
- **Lazy commit after `reserve`.** `reserve` writes only the control bytes
  and the overflow words (about 1.1 bytes per slot). The kernel commits the
  slot pages when inserts write to them.

"Growth memory (Linux)" and "Transparent huge pages (THP)" below give the
details.

### SIMD probing

Each slot has one control byte. A group of control bytes is compared with
one SIMD compare, so one instruction sequence checks 32 slots (AVX2) or 16
slots (SSE2, NEON) for the key's 8-bit fingerprint. The back end is chosen
at compile time, and the probe code is the same for all of them.

- **AVX2**: 32 slots per group. `vpcmpeqb` + `vpmovmskb`. The fingerprint
  is clamped in the vector unit (`vpminub`), with no scalar branch.
- **SSE2**: 16 slots per group, the same instructions on 128-bit registers.
- **NEON**: 16 slots per group. NEON has no `movemask`, so the map narrows
  the compare result with one `shrn` to a 64-bit mask.
- **SWAR**: 8 slots per group in a `uint64_t`, for any other CPU.

A find usually needs one group load, one compare and one key compare. The
8-bit fingerprint (254 values) gives about half the false matches of a
7-bit one. "SIMD group probing" below gives the details.

### Speed

Measured on the Xeon at 100M entries (`uint64_t -> uint64_t`, random keys,
ns/op, lower is better; see [BENCHMARK.md](BENCHMARK.md)):

| | sflat | boost_flat | absl_flat | dense_hash_map | std::unordered_map | boost_node | absl_node |
|---|---|---|---|---|---|---|---|
| insert (reserved) | **42.7** | 92.9 | 75.4 | 108.3 | 427.6 | 384.3 | 293.5 |
| find hit (random order) | 41.3 | 45.8 | 45.9 | **35.0** | 88.6 | 80.5 | 74.2 |
| find miss | **25.0** | 28.6 | 26.0 | 42.8 | 98.7 | 82.2 | 42.1 |
| insert (no reserve) | **90.6** | 142.5 | 143.5 | 266.1 | 670.3 | 459.9 | 351.3 |
| bytes/entry (steady) | **19.6** | 21.5 | 22.8 | 43.0 | 40.1 | 40.6 | 44.1 |

- **Against the other flat maps** the speed is close. At 100M sflat is the
  fastest on insert and find miss. dense_hash_map is 15 percent faster on
  find hit, but it uses 2.2 times the memory.
- **Against the node-based maps** (`std::unordered_map`, boost and absl
  node maps) sflat is much faster: reserved insert 7 to 10 times, insert
  without reserve 4 to 7 times, and find hit 1.8 to 2.2 times. It also
  uses about half the memory.
- google::sparse_hash_map is not in the table. It uses about 9 percent less
  memory than sflat, but its find hit is 4 to 6 times slower and its insert
  7 to 15 times slower.

"Benchmark" below summarizes the other sizes and machines.

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

`-march=native` enables AVX2 on CPUs that have it. Without AVX2 the map
uses SSE2, which every x86-64 CPU has. On ARM (for example Apple Silicon)
NEON is part of the base AArch64 ISA, so no flag is needed. `-mcpu=native`
is fine too:

`clang++ -std=c++17 -O3 -mcpu=native -I include your_file.cpp`

## File structure

```
include/sflat/flat_hash_map.hpp   # the only header you need
tests/correctness.cpp             # correctness tests
tests/growth_tiers.cpp            # growth factor tests
bench/bench.cpp                   # benchmark program
BENCHMARK.md                      # benchmark results for all machines
thirdparty/                       # comparison maps for the benchmark (not in git)
```

## Design

### Memory layout

The map has three arrays.

- **slots**: a dense array of `pair<const Key, T>`. Each slot is 16 bytes
  for `uint64_t -> uint64_t`.
- **ctrl**: one control byte per slot, in the SwissTable style. `0xFF`
  means empty. `0xFE` means deleted (a tombstone). Values in `[0, 0xFD]`
  hold the 8-bit hash fingerprint (H2).
- **overflow**: one 32-bit word per group, a Bloom filter for fast misses.
  See "Overflow word".

Memory per entry for `uint64_t -> uint64_t` at the 0.875 load factor:

```
bytes/entry = (16 + 1 + 4/32) / 0.875 = about 19.6   (AVX2)
bytes/entry = (16 + 1 + 4/16) / 0.875 = about 19.7   (SSE2, NEON)
```

The 4 is the 32-bit overflow word of each group. The measured values are
19.6 bytes/entry at 100M on the Xeon (AVX2) and 19.7 on the i5-3230M
(SSE2). Small tables measure up to about 22 bytes/entry because of
allocator overhead.

### SIMD group probing

The back end is chosen at compile time:

| Back end | When | Group | Match |
|----------|------|-------|-------|
| AVX2 | x86-64 with `__AVX2__` | 32 bytes | `vpcmpeqb` + `vpmovmskb`, 1 bit per lane |
| SSE2 | other x86-64, or `SFLAT_NO_AVX2=1` | 16 bytes | `pcmpeqb` + `pmovmskb`, 1 bit per lane |
| NEON | ARM with NEON (AArch64, Apple Silicon, ARMv7 + NEON), little endian | 16 bytes | `cmeq` + `shrn #4` + `fmov`, 4 bits per lane |
| SWAR | any other CPU, or `SFLAT_NO_SIMD=1` | 8 bytes | exact zero-byte test on a `uint64_t`, 8 bits per lane |

- **H2 clamp.** H2 is the low 8 bits of the mixed hash, clamped to
  `0xFD` so that it never equals the empty or deleted value. With AVX2 and
  SSE2 the clamp is one `vpminub` / `pminub` on the broadcast value, so the
  match needs no scalar compare and no branch.
- **NEON.** NEON has no `movemask` instruction. The map narrows the
  16-byte compare result with `shrn` (shift right by 4 and narrow each
  16-bit pair) to a 64-bit value with 4 bits per lane, and moves it to a
  general register. Masking with `0x1111...` keeps one bit per lane, so the
  usual `m &= m - 1` loop works and the lane index is `ctz(m) / 4`. As
  `ctz(m)` is a multiple of 4, the slot offset is one shift. A match is
  `dup`, `cmeq`, `shrn`, `fmov`, `and`.
- **Same code for all back ends.** The probe loops use `Group::Mask`,
  `Group::Lane()` and `Group::kAll`.
- **Linear probing by group.** A key that does not fit in its home group
  goes to the first later group with an empty slot.

**find.** The inlined part of `find()` is small. A DRAM-bound find loop
gets slower with every extra instruction, because fewer finds then fit in
the out-of-order window. The inlined part does this:

1. Compute the home group, load its control bytes, and (on x86-64) its
   overflow word. The two loads overlap.
2. Compare the group with H2. For each match, compare the key.
3. If the key's overflow bit is clear, the key is not in the table: return
   a miss. This is the usual case for a miss.
4. Else probe the next groups in `find_ptr_next()` (out of line on
   x86-64, inline on NEON). It stops at the first group with an empty slot.

During a migration, `find_ptr_slow()` also checks the old table.

**insert.** The fast path is inlined: it loads the home group, compares
H2, and writes the new element into the first empty slot of the home group.
All other cases (full home group, growth, migration) are in an out-of-line
function, so an insert loop holds only the fast path.

**Iteration** also scans a group at a time. A byte loop mispredicts on
almost every slot when full and free slots are mixed (for example after
many erases).

### Hash and position

The map processes the hash in three steps.

1. It takes `std::hash<Key>`. You can supply your own hash.
2. It applies a boost-style mulx64 avalanche: `(x * C) ^ ((x * C) >> 64)`.
   This is one 128-bit multiplication. It spreads weak hashes well, for
   example the identity hash of sequential integers. A plain 64-bit
   `x * C` is not sufficient. In a 64-bit product, bit k depends only on
   bits [0, k] of the key, so H2 depends only on the low 8 bits of the key.
   Keys with regular low bits (aligned pointers, `i << 8`, `i * 64`) then
   get one or a few H2 values, and find becomes up to 10 times slower. The
   xor of the high half prevents this. `test_hash_bits_structured_keys`
   checks it.
3. It splits the mixed hash into three parts that do not overlap:
   - bits [32, 64): the home group, with Lemire fastrange
     (`(groups * (h >> 32)) >> 32`). It does not need a power-of-two group
     count, unlike a bitmask. `find` and `insert` compute the first slot of
     the home group with one high multiply and a mask.
   - bits [0, 8): H2.
   - bits [8, 13): the overflow bit (bits [8, 8 + log2(SFLAT_OVERFLOW_BITS))
     in general).

Fastrange is monotonic in the hash: a key has its home at the same
relative position in a table of any size. The incremental growth relies on
this (see "Growth memory").

### Overflow word (fast miss)

Each group has a 32-bit Bloom filter. When an insert puts a key into a
later group because the home group is full, the map sets one bit (chosen by
the hash) in the home group's word. If `find` does not locate the key in
the home group and the key's bit is clear, the key is not in the table: the
miss ends after one group. The idea comes from `boost::unordered_flat_map`,
which keeps an 8-bit filter inside each group.

Why 32 bits: at the 0.875 load factor about 48 percent of the home groups
are full. With an 8-bit filter, about 20 percent of the misses in a full
group find their bit set by chance, so 9.5 percent of all misses probed a
second group. Such a miss is expensive: its branch waits for a DRAM load,
so a mispredict costs about one memory latency (about 140 ns per probing
miss on the i5-3230M, half of the total miss time). With 32 bits the share
drops to 2.7 percent (64 bits: 1.4 percent). On the i5-3230M, misses got 12
to 20 percent faster, hits did not change, and the cost is 0.19 bytes per
slot (about 1 percent). A filter inside the control cache line (as in
boost) was also tried. It saved no time, because the extra address
arithmetic slowed down hits (see BENCHMARK.md, "Profiling on the
i5-3230M").

On x86-64, `find` reads the overflow word together with the control bytes,
so the two loads overlap. On NEON it reads the word only when no key in the
home group matched (`SFLAT_LAZY_OVERFLOW`): on Apple M2 this made random
hits 8 to 13 percent faster.

`erase` does not clear overflow bits. This is conservative and correct.
A rehash or a migration rebuilds the bits.

### Capacity and growth

- The capacity is always a multiple of the group width (32, 16 or 8). It
  does not need to be a power of two. `reserve(n)` gives
  `ceil(n / max_load_factor)` slots, rounded up to the group width.
- The default max load factor is 0.875. You can change it with
  `max_load_factor()`.
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
- **Incremental growth.** The map does not move all items at once. Each
  insert during a migration moves the next 64 old slots (2 groups with
  AVX2) to the new table. This avoids a latency spike: at 10M entries the
  worst insert takes 4 to 50 ms, against 150 to 400 ms for boost_flat and
  emhash8, which rehash in one insert.
- During a migration, `find` checks the new table first and then the old
  table. `find`, `erase` and iteration never move items. Only inserts move
  the migration forward. `finish_migration()` completes it at once.

### Growth memory (Linux)

With `std::allocator` on Linux, each array of 2 MiB or more gets its own
2 MiB-aligned `mmap`. Smaller arrays use `operator new` with 64-byte
alignment, so a control group never crosses two cache lines.

A plain incremental rehash keeps the full old table (1x) and the full new
table (1.25x to 2x) alive until the migration ends. The map avoids this
peak in three steps:

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

- control bytes, overflow words, and slots after `reserve()`: at once;
- slots of a growing table: a 2 MiB page just ahead of the migration front.
  Pages far ahead of the front stay `MADV_NOHUGEPAGE`, so a rare stray write
  does not commit a full 2 MiB page;
- at the end of a migration, `MADV_COLLAPSE` (Linux 6.1 and later) converts
  the few 4 KiB pages that were written before their hint.

The system THP setting must be `madvise` or `always`
(`/sys/kernel/mm/transparent_hugepage/enabled`). With `never`, the hints
have no effect.

THP helps most when the table is much larger than the last-level cache.
On the Xeon (260 MiB L3) at 100M entries, THP made find hit 18 percent
faster. At 1M entries the gain is small, and the huge page faults can make
inserts slower in a virtual machine (see BENCHMARK.md).

On macOS and other non-Linux systems `SFLAT_USE_MMAP` defaults to 0: all
arrays come from `operator new`, so there is no page release and no THP.
Incremental growth still works (no latency spike), but the growth peak is
old table + new table. A custom allocator also disables `mmap` and THP: the
map then uses only the allocator.

### Configuration macros

Define these before the include.

| Macro | Default | Effect |
|-------|---------|--------|
| `SFLAT_USE_MMAP` | 1 on Linux | 0: use `operator new` for all arrays. No page release, no THP. |
| `SFLAT_USE_THP` | 1 | 0: keep `mmap` and page release, but give no huge page hints. |
| `SFLAT_GROW_2X_BELOW` | 2 GiB | Tables smaller than this many bytes grow 2x. |
| `SFLAT_GROW_1_5X_BELOW` | 10 GiB | Tables smaller than this grow 1.5x; larger tables grow 1.25x. |
| `SFLAT_NO_SIMD` | 0 | 1: use the portable SWAR group (8 bytes) instead of AVX2, SSE2 or NEON. |
| `SFLAT_NO_AVX2` | 0 | 1: use SSE2 groups (16 bytes) in an AVX2 build. |
| `SFLAT_OVERFLOW_BITS` | 32 | Bits per group in the overflow Bloom filter: 8, 16, 32 or 64. See "Overflow word". |
| `SFLAT_PREFETCH_SLOTS` | 1 with NEON, else 0 | 1: `find()` prefetches the home group's slots before it reads the control bytes. |
| `SFLAT_LAZY_OVERFLOW` | 1 with NEON, else 0 | 1: `find()` reads the overflow word only when no key in the home group matched. |
| `SFLAT_FIND_NEXT_INLINE` | 1 with NEON, else 0 | 1: the probe of the group after the home group is inlined in `find()`. |

## API compatibility

The map provides the common `std::unordered_map` interface. This includes
constructors, copy and move, swap, `insert`, `emplace`, `try_emplace`,
`insert_or_assign`, `operator[]`, `at`, `find`, `count`, `contains`,
`erase`, `clear`, iterators, `reserve`, `rehash`, `load_factor`,
`max_load_factor`, `bucket_count`, `bucket_size`, `hash_function`,
`key_eq`, `get_allocator`, and comparison operators.

Extra functions:

- `optimize()`: use it when the map will have no more inserts or erases. It
  completes a migration, removes tombstones, and shrinks to the minimal
  capacity. The gain is modest (about 5 percent with 30 percent
  tombstones): at large sizes the cost is the cache misses, not the probe
  length. `shrink_to_fit()` does the same resize.
- `finish_migration()`: completes an ongoing migration at once.
- `find_batch(keys, n, out_found, out_values)`: looks up `n` keys. It
  prefetches the home groups first to overlap the DRAM accesses. The gain
  was small in an older version (1.04x at 100M entries, none at 10M),
  because the CPU already overlaps misses well in a simple loop.

Limits (common trade-offs of open addressing):

- Iterators are not node-based. An insert can move elements (a rehash, or
  a migration step), and then iterators and pointers become invalid.
  `std::unordered_map` guarantees that `insert` does not invalidate
  iterators. This map does not give that guarantee.
- `erase` never moves other items. `it = m.erase(it)` loops are safe, also
  during a migration. `erase` leaves a tombstone; after many erases, call
  `rehash` or `optimize()`.
- `const` functions never change the map, also during a migration. Many
  threads can read at the same time while no thread writes.
- Bucket and local iterators do not have full standard semantics.
  `begin(size_t)` and related functions are approximate.
- Capacity limit: fewer than 2^32 groups, because the position function
  (fastrange) is 32-bit. With AVX2 (32 slots per group) this is about 137
  billion slots, or 120 billion entries at the 0.875 load factor. With SSE2
  and NEON it is half, and with SWAR a quarter. `max_size()` returns the
  limit. Above it, `insert`, `reserve` and `rehash` throw
  `std::length_error`.

## Benchmark

The full results are in [BENCHMARK.md](BENCHMARK.md). It covers four
machines: an Intel Xeon KVM virtual machine (AVX2), an Intel Celeron N5105
and an Intel Core i5-3230M on bare metal (SSE2), and an Apple M2 (NEON). It
also has the build and run steps.

In short (`uint64_t -> uint64_t`, random keys, compared with emhash8,
boost::unordered_flat_map, absl::flat_hash_map, the node-based maps, and on
the Xeon also Google dense_hash_map and sparse_hash_map):

- **Memory**: sflat uses the least memory in steady state on every machine
  (19.5 to 21.8 bytes/entry), except Google sparse_hash_map on the Xeon.
  On Linux at 50M to 100M entries sflat also has the lowest growth peak,
  again except sparse_hash_map.
- **Find hit (random order)**: on Linux x86-64, sflat is the fastest at
  the largest size of each machine, except dense_hash_map on the Xeon. At
  1M on the Xeon, boost_flat is 11 percent faster. On the M2 (no THP)
  absl_flat and boost_flat are faster at 10M and 100M.
- **Insert**: on the Celeron and the i5, sflat is the fastest in most
  rows. On the Xeon VM, sflat is the fastest at 1M and 100M, and
  boost_flat is 3 percent faster at 10M (reserved insert). On the M2,
  boost_flat is the fastest.
- On the machines with small caches and TLBs (Celeron, i5) much of the lead
  of sflat comes from transparent huge pages, not from the probing scheme.
- **Find miss**: on the Xeon at 100M sflat is the fastest. Otherwise
  boost_flat or absl_flat is faster: 1.1 to 1.3 times on the Xeon at 1M
  and 10M and on the i5-3230M.
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
`std::unordered_map`, string keys, sequential keys, hash bits of keys with
regular low bits, copy and move, swap, iterator erase, reserve and rehash,
operations during a migration, large tables on the mmap and THP path, a
user allocator, the capacity limit, and growth factor checks.
`growth_tiers.cpp` sets small tier limits and checks the 2x, 1.5x and 1.25x
steps. `test_group_match` checks the SIMD back end against a scalar
reference for every lane and every byte value.

All tests pass on each back end:

| Back end | How it was tested |
|----------|-------------------|
| AVX2 | x86-64 Linux, `-march=native`, also with ASan and UBSan; `clang++ -arch x86_64 -mavx2` under Rosetta 2 on Apple M2 |
| SSE2 | `-msse4.2` (no AVX2) on x86-64 Linux, also with ASan and UBSan; `clang++ -arch x86_64` under Rosetta 2 |
| NEON | Apple M2, macOS 26.6, Apple clang 21, also with ASan and UBSan |
| SWAR | `-DSFLAT_NO_SIMD=1` on x86-64 Linux and on Apple M2, also with ASan and UBSan |

## License

This project uses the Apache License 2.0. The LICENSE file has the full
text. Each source file has an `SPDX-License-Identifier: Apache-2.0` line.

The license covers `include/`, `tests/`, and `bench/`. `thirdparty/` holds
the comparison maps for the benchmark. It is not in git, and each item
keeps its original license (emhash, Boost, Abseil, sparsehash).
