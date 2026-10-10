# sflat::flat_hash_map

sflat::flat_hash_map is a C++17 header-only hash map. It uses open
addressing. The interface matches the common subset of std::unordered_map.

The design has three goals. First, insert and find are fast. Second, the
growth factor gets smaller as the table gets large (2x, 1.5x, 1.25x). This
uses less memory for large tables. Third, growth is
incremental and gives old pages back to the kernel. This avoids a latency
spike and keeps the growth memory peak low. The map suits large data sets
where memory is important.

## File structure

```
include/sflat/flat_hash_map.hpp   # the only header you need
tests/correctness.cpp             # correctness tests
bench/bench.cpp                   # benchmark program
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

## Design

### Memory layout

The map has three arrays.

- **slots**: a dense array of `pair<const Key, T>`. Each slot is 16 bytes
  for `uint64_t -> uint64_t`.
- **ctrl**: a control array with one byte per slot. The style follows
  SwissTable. `0xFF` means empty. `0xFE` means deleted. Values in
  `[0, 0xFD]` hold the 8-bit hash fingerprint (H2).
- **overflow**: one byte per 32 slots. Each byte is a Bloom filter. See
  below.

### Capacity and growth

- The capacity is always a multiple of the SIMD group width. The width is
  32 with AVX2 and 16 with SSE2. The capacity does not need to be a power
  of two.
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
bytes/entry = (16 + 1 + 1/32) / 0.875 = about 19.5
```
The measured value is 19.5 to 20.7 bytes/entry. The difference is allocator
overhead.

### Growth memory (Linux)

With `std::allocator` on Linux, each array of 2 MiB or more gets its own
2 MiB-aligned `mmap`. Smaller arrays use `operator new` with 64-byte
alignment, so a control group never crosses two cache lines.

A plain incremental rehash keeps the full old table (1x) and the full new
table (1.5x) alive until the migration ends. The map avoids this peak in
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

A custom allocator disables both. The map then uses only the allocator.

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

- With AVX2, each group has 32 control bytes. One `vpcmpeqb` plus one
  `vpmovmskb` compares the H2 values.
- Without AVX2, the code uses the SSE2 16-byte group path.
- Probing is linear by group.

### Hash strategy

The map processes the hash in three steps.

1. It takes `std::hash<Key>`. You can supply your own hash.
2. It applies a boost-style mulx64 avalanche: `(x * C) ^ ((x * C) >> 64)`.
   This is one 128-bit multiplication. It spreads weak hashes well, for
   example the identity hash of sequential integers.
3. It uses the high 32 bits of the mixed hash for fastrange positioning.
   It uses the low 8 bits for H2. It uses bits [8, 11) for the overflow bit.
   The three parts do not overlap. This reduces correlation.

### Overflow byte (fast miss)

Each group has a one-byte Bloom filter. When a key overflows from its home
group to a later group because the home group is full, the map sets a bit
in the home group. When `find` does not locate the key in the home group,
it checks the bit. If the bit is clear, `find` returns a miss at once. It
does not scan further. This technique comes from `boost::unordered_flat_map`.
It improves miss speed by about 33 percent.

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
  it is half. `max_size()` returns the limit. Above it, `insert`,
  `reserve` and `rehash` throw `std::length_error`.

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

### Test environment

- CPU: Intel Xeon @ 2.10 GHz (4 vCPU, AVX2 and AVX-512, 260 MiB L3),
  RAM 15 GiB. The machine is a VM.
- Kernel: Linux 6.18. THP setting `madvise`.
- Compiler: g++ 13.3.0 with `-O3 -march=native -std=c++17`.
- Comparison targets: emhash8 (upstream commit 801d02a, load factor 0.80),
  boost::unordered_flat_map and boost::unordered_map (Boost 1.83),
  absl::flat_hash_map and absl::node_hash_map (Abseil LTS 20260817.0),
  std::unordered_map (libstdc++ 13). They use glibc malloc without THP.
- All maps are compiled with `-DNDEBUG`, so no debug asserts run.
- Test: `uint64_t -> uint64_t` with random keys. Each map runs in its own
  child process. Memory is the child peak RSS minus the baseline (the input
  arrays only).
- Before each size, a warm-up child touches and frees memory. In a VM the
  host backs guest memory on first use, which is very slow. Without the
  warm-up, the first map in the run order pays this cost.
- 1M and 10M: median of 3 runs. 100M: 1 run, from an earlier run without
  Abseil and without `-DNDEBUG`.

### Results

Lower ns/op is better. Lower bytes/entry is better. **Bold** is the best
value in the row.

- "insert (reserved)" includes the `reserve(n)` call.
- "find hit (insert order)" asks for the keys in insertion order. This
  favors emhash8, which stores the elements densely in insertion order: its
  element reads become sequential. "find hit (random order)" is the fair
  test.
- "growth peak" is the peak RSS of the no-reserve test.

**n = 1,000,000**

| Operation | sflat | emhash8 | boost_flat | boost_node | absl_flat | absl_node | std |
|---|---|---|---|---|---|---|---|
| insert (reserved) | **29.6** | 35.6 | 30.3 | 94.0 | 35.7 | 66.9 | 109.9 |
| find hit (insert order) | 9.9 | **8.8** | 9.9 | 21.9 | 8.9 | 12.5 | 28.1 |
| find hit (random order) | 10.9 | 12.9 | 9.9 | 21.6 | **9.0** | 19.8 | 30.3 |
| find miss | 14.5 | 12.3 | **6.1** | 20.4 | 9.2 | 10.3 | 37.8 |
| erase | 14.2 | 21.9 | **11.0** | 43.9 | 15.1 | 33.2 | 70.2 |
| iterate | 12.2 | **0.5** | 10.5 | 15.2 | 11.7 | 13.8 | 24.5 |
| insert (no reserve) | 71.1 | 86.4 | **54.8** | 139.8 | 57.1 | 88.7 | 182.3 |
| bytes/entry (steady) | **21.3** | 34.0 | 34.8 | 46.6 | 36.9 | 52.1 | 41.8 |
| bytes/entry (growth peak) | **29.4** | 36.0 | 51.1 | 46.6 | 54.2 | 58.3 | 45.2 |

**n = 10,000,000**

| Operation | sflat | emhash8 | boost_flat | boost_node | absl_flat | absl_node | std |
|---|---|---|---|---|---|---|---|
| insert (reserved) | 37.3 | 57.3 | **35.0** | 170.6 | 48.4 | 101.2 | 220.0 |
| find hit (insert order) | 21.5 | **18.6** | 24.6 | 37.8 | 28.8 | 25.0 | 52.6 |
| find hit (random order) | **24.9** | 31.5 | 26.4 | 54.8 | 29.6 | 43.4 | 67.8 |
| find miss | 20.4 | 22.2 | **10.6** | 47.7 | 21.3 | 24.7 | 77.9 |
| erase | **31.0** | 54.1 | **31.0** | 110.9 | 46.0 | 81.5 | 194.1 |
| iterate | 12.1 | **1.0** | 11.6 | 30.5 | 10.4 | 18.3 | 78.2 |
| insert (no reserve) | 166.1 | 118.1 | **56.9** | 235.8 | 61.0 | 133.5 | 367.7 |
| bytes/entry (steady) | **19.9** | 29.6 | 27.0 | 42.8 | 28.6 | 47.2 | 40.4 |
| bytes/entry (growth peak) | **27.3** | 29.6 | 40.3 | 42.8 | 42.9 | 47.2 | 41.9 |

At 10M the sflat insert rows vary a lot between runs in this VM (reserved:
37 to 90 ns, no reserve: 124 to 177 ns). The cause is the huge page faults
(see "THP on and off" below).

**n = 100,000,000** (1 run, without Abseil)

| Operation | sflat | emhash8 | boost_flat | boost_node | std |
|-----------|-------|---------|------------|------------|-----|
| insert (reserved) | **76.5** | 145.6 | 78.5 | 348.2 | 369.3 |
| find hit (insert order) | 43.9 | **31.9** | 41.9 | 56.6 | 68.9 |
| find hit (random order) | 43.0 | 49.9 | **42.1** | 75.1 | 90.6 |
| find miss | 32.5 | 34.4 | **20.4** | 71.8 | 99.3 |
| erase | 63.6 | 76.5 | **48.5** | 174.1 | 298.1 |
| iterate | 13.3 | **1.1** | 11.3 | 43.4 | 125.4 |
| insert (no reserve) | 159.8 | 233.6 | **131.1** | 435.1 | 641.0 |
| bytes/entry (steady) | **19.5** | 26.7 | 21.5 | 40.6 | 40.1 |
| bytes/entry (growth peak) | **25.2** | 26.7 | 32.2 | 40.6 | 40.1 |

### Insert latency during growth

Insert 10M keys with no `reserve`. Each insert is timed.

| Map | total | p99.9 | p99.99 | max |
|-----|-------|-------|--------|-----|
| sflat | 2.8 s | 2.2 us | 21 us | **30 ms** |
| boost_flat | **2.1 s** | **0.5 us** | **15 us** | 163 ms |
| emhash8 | 3.4 s | 2.3 us | 19 us | 267 ms |

boost_flat and emhash8 rehash the full table in one insert. sflat spreads
the rehash over many inserts. Its worst case is 5 to 9 times smaller. The
remaining 30 ms is mostly huge page faults and the final `MADV_COLLAPSE`.

### THP on and off (sflat only)

THP cuts TLB misses but makes each first touch of a 2 MiB page slower. This
VM uses free page reporting, so a new huge page often needs host work too.
On bare metal a huge page fault usually costs only the time to clear 2 MiB.

| sflat, ns/op | 10M, THP off | 10M, THP on | 100M, THP off | 100M, THP on |
|--------------|--------------|-------------|---------------|--------------|
| insert (reserved) | **46 to 50** | 88 to 97 | 98.9 | **76.5** |
| find hit (random order) | 31 to 33 | **27 to 31** | 51.6 | **43.0** |
| insert (no reserve) | **99 to 102** | 108 to 167 | 197.0 | **159.8** |

At 100M, THP is better in all rows. At 10M the table fits in the 260 MiB L3
of this CPU, so the TLB gain is small and the fault cost is larger. To turn
THP off, define `SFLAT_USE_THP=0`.

### Honest assessment

- **memory (steady state)**: sflat uses the least memory at all sizes: 9 to
  39 percent less than boost_flat, 30 to 42 percent less than absl_flat, and
  27 to 37 percent less than emhash8.
- **memory (growth peak)**: sflat now has the lowest peak at all sizes (25
  to 29 bytes/entry). Before page release it was 39 to 49.
- **insert (reserved)**: sflat is the fastest at 1M (29.6 ns) and 100M
  (76.5 ns). At 10M the median is 37.3 ns, near boost_flat (35.0), but THP
  faults in this VM make single runs as slow as 90 ns.
- **find hit (random order)**: sflat is the fastest at 10M (24.9 ns). At
  1M absl_flat (9.0) and boost_flat (9.9) are faster than sflat (10.9).
  sflat is faster than emhash8 at all sizes.
- **find miss**: boost_flat is still 1.6 to 2.4 times faster. absl_flat
  is faster at 1M and equal at 10M. sflat fills the table to 0.875, so the overflow
  bits are often set.
- **insert (no reserve)**: boost_flat and absl_flat are faster (1.2x to 3x).
  With 1.5x growth each element moves about twice; with 2x growth about
  once.
- **erase and iterate**: not a goal. Both are within 1.3x of boost_flat.

### Large-scale estimates (not measured)

The table below extrapolates from the measured steady-state bytes/entry.

| Entries | sflat (about 20 B/entry) | boost_flat (about 30 B/entry) |
|---------|--------------------------|--------------------------|
| 100M    | about 2.0 GB             | about 3.0 GB             |
| 1B      | about 20 GB              | about 30 GB              |
| 100B    | about 2.0 TB             | about 3.0 TB             |

With page release, the growth peak is about the size of the new table
(1.5x of the old table) plus the control bytes. The measured peak is 25 to
29 bytes/entry. The actual values depend on the allocator and on the key
and value sizes.

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
and 1.25x steps. All tests also pass with `-mno-avx2` (SSE2 path) and with
ASan and UBSan.

## License

This project uses the GNU General Public License v3.0. The LICENSE file
has the full text.

The license covers `include/`, `tests/`, and `bench/`. `thirdparty/` holds
third-party code. Each third-party item keeps its original license (emhash,
Boost).
