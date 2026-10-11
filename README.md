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
- **overflow**: one 16-bit word per group (32 slots with AVX2, 16 with
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
bytes/entry = (16 + 1 + 2/32) / 0.875 = about 19.5
```
The measured value is 19.5 to 21.6 bytes/entry. The difference is allocator
overhead. With 16-byte groups (SSE2, NEON) the overflow term is 1/16, which
adds about 0.04 bytes/entry.

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

- control bytes, overflow words, and slots after `reserve()`: at once;
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
   It uses the low 8 bits for H2. It uses bits [8, 12) for the overflow bit.
   The three parts do not overlap. This reduces correlation.

### Overflow word (fast miss)

Each group has a 16-bit Bloom filter. When a key overflows from its home
group to a later group because the home group is full, the map sets a bit
in the home group. When `find` does not locate the key in the home group,
it checks the bit. If the bit is clear, `find` returns a miss at once. It
does not scan further. This technique comes from `boost::unordered_flat_map`.
It improves miss speed by about 33 percent.

boost uses 8 bits for 15 slots. sflat groups have 32 slots (AVX2), so 8 bits
fill up fast. At load 0.875 one miss in three finds its home group full.
With 8 bits, 72 percent of these misses found the bit set and probed the
next group. With 16 bits, the extra groups per miss go from 0.24 to 0.14
and a miss is 5 to 8 percent faster (1M and 10M, after `reserve`). Insert
and find hit do not change. The cost is one more byte per 32 slots.

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

The benchmark has been run on three machines:

- [Intel Xeon (Emerald Rapids), KVM virtual machine](#intel-xeon-emerald-rapids-kvm-virtual-machine):
  AVX2 path, large L3 cache, 1M to 100M entries.
- [Intel Celeron N5105, bare metal](#intel-celeron-n5105-bare-metal):
  SSE2 path (no AVX2), small caches and TLB, 1M to 50M entries.
- [Apple M2, MacBook Air (macOS)](#apple-m2-macbook-air-macos):
  NEON path (ARM), 128-byte cache lines, 16 KiB pages, 1M to 100M entries.

All use the same program (`bench/bench.cpp`). The Xeon and the Celeron use
the same versions of the comparison maps; the M2 uses the versions listed
in its section.

### Intel Xeon (Emerald Rapids), KVM virtual machine

#### Test environment

| Item | Value |
|------|-------|
| Machine | KVM virtual machine (cloud) |
| CPU | Intel Xeon @ 2.10 GHz, family 6 model 207 (5th gen Xeon Scalable, Emerald Rapids) |
| Cores | 4 vCPU (4 cores, 1 thread per core, 1 socket) |
| ISA | SSE4.2, AVX2, BMI2, AVX-512 |
| Cache | L1d 48 KiB per core, L2 2 MiB per core, L3 260 MiB shared |
| Memory | 15 GiB, no swap |
| OS | Ubuntu 24.04.5 LTS |
| Kernel | Linux 6.18.44 |
| THP | `madvise` (defrag `madvise`); free page reporting on |
| C library | glibc 2.39 |
| Compiler | g++ 13.3.0, `-O3 -march=native -std=c++17 -DNDEBUG` |

- Comparison targets: emhash8 (upstream commit 801d02a, load factor 0.80),
  boost::unordered_flat_map and boost::unordered_map (Boost 1.83),
  absl::flat_hash_map and absl::node_hash_map (Abseil LTS 20260817.0),
  std::unordered_map (libstdc++ 13). They use glibc malloc without THP.
- All maps are compiled with `-DNDEBUG`, so no debug asserts run. (The THP
  and latency tables come from earlier runs.)
- Test: `uint64_t -> uint64_t` with random keys. Each map runs in its own
  child process. Memory is the child peak RSS minus the baseline (the input
  arrays only).
- Before each size, a warm-up child touches and frees memory. In a VM the
  host backs guest memory on first use, which is very slow. Without the
  warm-up, the first map in the run order pays this cost.
- 1M and 10M: median of 3 runs. 100M: 1 run.

#### Results

Lower ns/op is better. Lower bytes/entry is better. **Bold** is the best
value in the row.

- "insert (reserved)" includes the `reserve(n)` call.
- "find hit (insert order)" asks for the keys in insertion order. This
  favors emhash8, which stores the elements densely in insertion order: its
  element reads become sequential. "find hit (random order)" is the fair
  test.
- "growth peak" is the peak RSS of the no-reserve test.
- sflat grows 2x below 2 GiB, so all three sizes here use the 2x tier.

**n = 1,000,000**

| Operation | sflat | emhash8 | boost_flat | boost_node | absl_flat | absl_node | std |
|---|---|---|---|---|---|---|---|
| insert (reserved) | 74.4 | 37.3 | **27.3** | 100.4 | 38.5 | 76.8 | 122.5 |
| find hit (insert order) | 10.6 | 9.5 | **9.2** | 22.6 | 12.0 | 15.4 | 33.7 |
| find hit (random order) | 10.6 | 13.9 | **8.9** | 24.6 | 11.3 | 21.2 | 36.0 |
| find miss | 18.2 | 12.6 | **5.6** | 22.7 | 9.6 | 10.9 | 41.0 |
| erase | 14.7 | 22.5 | **11.1** | 49.7 | 16.7 | 31.9 | 78.9 |
| iterate | 11.9 | **0.6** | 10.6 | 16.7 | 10.0 | 12.3 | 25.7 |
| insert (no reserve) | 139.4 | 101.5 | **60.8** | 156.2 | 66.6 | 104.7 | 196.0 |
| bytes/entry (steady) | **21.4** | 34.1 | 34.9 | 46.7 | 37.0 | 52.2 | 41.9 |
| bytes/entry (growth peak) | 40.6 | **36.1** | 51.2 | 46.7 | 54.2 | 58.4 | 45.2 |

**n = 10,000,000**

| Operation | sflat | emhash8 | boost_flat | boost_node | absl_flat | absl_node | std |
|---|---|---|---|---|---|---|---|
| insert (reserved) | 82.4 | 56.2 | **35.1** | 176.3 | 50.2 | 97.3 | 212.9 |
| find hit (insert order) | 23.1 | **19.5** | 25.0 | 39.4 | 28.3 | 25.7 | 51.5 |
| find hit (random order) | 26.3 | 32.9 | **25.3** | 55.1 | 28.0 | 44.6 | 64.7 |
| find miss | 22.2 | 20.6 | **10.8** | 49.4 | 21.1 | 22.6 | 76.8 |
| erase | 32.6 | 47.4 | **31.7** | 115.9 | 46.5 | 85.0 | 186.3 |
| iterate | 11.7 | **1.0** | 11.5 | 32.3 | 10.3 | 19.1 | 76.6 |
| insert (no reserve) | 93.3 | 115.6 | **54.7** | 245.2 | 63.3 | 136.7 | 396.2 |
| bytes/entry (steady) | **19.9** | 29.6 | 27.0 | 42.8 | 28.7 | 47.2 | 40.4 |
| bytes/entry (growth peak) | 29.9 | **29.6** | 40.4 | 42.8 | 42.9 | 47.2 | 41.9 |

The sflat insert rows vary a lot between runs in this VM because of huge
page faults (see "THP on and off" below). At 1M, reserved insert was 58 to
96 ns and no-reserve insert 61 to 183 ns. With `SFLAT_USE_THP=0` the same
rows were stable: 30 to 33 ns and 70 to 85 ns. At 10M, reserved insert was
37 to 84 ns.

At 1M the no-reserve table lands just after a 2x step (2.1M slots, load
0.48). This is the reason for the 40.6 bytes/entry growth peak. It is the
cost of 2x growth for small tables.

**n = 100,000,000**

| Operation | sflat | emhash8 | boost_flat | boost_node | absl_flat | absl_node | std |
|---|---|---|---|---|---|---|---|
| insert (reserved) | 82.5 | 149.6 | 95.2 | 365.4 | **68.0** | 289.7 | 385.9 |
| find hit (insert order) | 41.0 | **32.2** | 43.2 | 55.1 | 40.2 | 40.9 | 71.2 |
| find hit (random order) | **39.0** | 51.5 | 44.3 | 78.8 | 42.3 | 66.6 | 91.1 |
| find miss | 29.7 | 34.6 | **21.1** | 70.5 | 41.4 | 48.1 | 100.2 |
| erase | 54.8 | 80.1 | **50.0** | 154.3 | 69.7 | 124.8 | 268.8 |
| iterate | 11.7 | **1.1** | 11.1 | 47.0 | 10.4 | 25.3 | 120.8 |
| insert (no reserve) | **110.6** | 224.0 | 131.0 | 447.3 | 123.9 | 320.1 | 638.0 |
| bytes/entry (steady) | **19.5** | 26.7 | 21.5 | 40.6 | 22.8 | 44.1 | 40.1 |
| bytes/entry (growth peak) | **23.6** | 26.7 | 32.2 | 40.6 | 34.2 | 44.1 | 40.1 |

100M: 1 run.

#### Insert latency during growth

Insert 10M keys with no `reserve`. Each insert is timed. (Measured with the
older 1.5x growth rule.)

| Map | total | p99.9 | p99.99 | max |
|-----|-------|-------|--------|-----|
| sflat | 2.8 s | 2.2 us | 21 us | **30 ms** |
| boost_flat | **2.1 s** | **0.5 us** | **15 us** | 163 ms |
| emhash8 | 3.4 s | 2.3 us | 19 us | 267 ms |

boost_flat and emhash8 rehash the full table in one insert. sflat spreads
the rehash over many inserts. Its worst case is 5 to 9 times smaller. The
remaining 30 ms is mostly huge page faults and the final `MADV_COLLAPSE`.

#### THP on and off (sflat only)

THP cuts TLB misses but makes each first touch of a 2 MiB page slower. This
VM uses free page reporting, so a new huge page often needs host work too.
On bare metal a huge page fault usually costs only the time to clear 2 MiB.
(Measured with the older 1.5x growth rule.)

| sflat, ns/op | 10M, THP off | 10M, THP on | 100M, THP off | 100M, THP on |
|--------------|--------------|-------------|---------------|--------------|
| insert (reserved) | **46 to 50** | 88 to 97 | 98.9 | **76.5** |
| find hit (random order) | 31 to 33 | **27 to 31** | 51.6 | **43.0** |
| insert (no reserve) | **99 to 102** | 108 to 167 | 197.0 | **159.8** |

At 100M, THP is better in all rows. At 1M and 10M the table fits in the
260 MiB L3 of this CPU, so the TLB gain is small and the fault cost is
larger. To turn THP off, define `SFLAT_USE_THP=0`.

#### Honest assessment

- **memory (steady state)**: sflat uses the least memory at all sizes: 9 to
  39 percent less than boost_flat, 14 to 42 percent less than absl_flat, and
  27 to 37 percent less than emhash8.
- **memory (growth peak)**: at 100M sflat has the lowest peak (23.6
  bytes/entry). At 10M it is equal to emhash8 (29.9 and 29.6). At 1M the 2x
  step makes it 40.6, more than emhash8 (36.1).
- **insert (no reserve)**: at 100M sflat is now the fastest (110.6 ns; was
  159.8 with 1.5x growth). At 10M it went from 166 to 93 ns. boost_flat is
  still faster at 1M and 10M.
- **insert (reserved)**: at 100M absl_flat is the fastest (68.0), then sflat
  (82.5). At 1M and 10M the THP fault noise in this VM hides the real speed;
  with THP off sflat is close to boost_flat.
- **find hit (random order)**: sflat is the fastest at 100M (39.0 ns) and
  within 4 to 19 percent of the best at 1M and 10M.
- **find miss**: boost_flat is still 1.4 to 3.3 times faster. sflat fills
  the table to 0.875, so the overflow bits are often set.
- **erase and iterate**: not a goal. Both are within 1.4x of boost_flat.

### Intel Celeron N5105, bare metal

#### Test environment

| Item | Value |
|------|-------|
| Machine | Bare-metal mini PC (no virtualization) |
| CPU | Intel Celeron N5105 @ 2.00 GHz, family 6 model 156 (Jasper Lake, Tremont cores) |
| Cores | 4 cores, 1 thread per core, 1 socket |
| ISA | SSE4.2, AES-NI, SHA-NI, GFNI. **No AVX, AVX2 or BMI2**, so sflat uses the SSE2 16-slot group path |
| Cache | L1d 32 KiB per core, L2 1.5 MiB shared, L3 4 MiB shared |
| Memory | 7 GiB usable, 1 GiB swap |
| OS | Ubuntu 26.04.1 LTS |
| Kernel | Linux 7.0.0-31-generic |
| THP | `madvise` (defrag `madvise`) |
| CPU frequency | `intel_pstate`, governor `powersave` (HWP) |
| C library | glibc 2.43 |
| Compiler | g++ 15.2.0, `-O3 -march=native -std=c++17 -DNDEBUG` |

- Comparison targets: emhash8 (upstream commit 801d02a, load factor 0.80),
  boost::unordered_flat_map and boost::unordered_map (Boost 1.83),
  absl::flat_hash_map and absl::node_hash_map (Abseil LTS 20260817.0),
  std::unordered_map (libstdc++ 15). They use glibc malloc without THP.
- All maps are compiled with `-DNDEBUG`, so no debug asserts run.
- Test: `uint64_t -> uint64_t` with random keys. Each map runs in its own
  child process. Memory is the child peak RSS minus the baseline (the input
  arrays only).
- Before each size, a warm-up child touches and frees memory. (This matters
  most in a VM, where the host backs guest memory on first use.)
- 1M and 10M: median of 3 runs. The runs agree well on this bare-metal
  machine: the sflat rows vary by less than 2 percent, except no-reserve
  insert at 1M (141 to 156 ns). 50M: 1 run.
- 100M is not measured. The four input arrays alone take 3.2 GB, and the
  node-based maps need another 4 GB. This does not fit in 7 GiB.

#### Results

Lower ns/op is better. Lower bytes/entry is better. **Bold** is the best
value in the row.

- "insert (reserved)" includes the `reserve(n)` call.
- "find hit (insert order)" asks for the keys in insertion order. This
  favors emhash8, which stores the elements densely in insertion order: its
  element reads become sequential. "find hit (random order)" is the fair
  test.
- "growth peak" is the peak RSS of the no-reserve test.
- sflat grows 2x below 2 GiB, so all three sizes here use the 2x tier.
- The L3 cache is only 4 MiB. Even the 1M tables (about 20 to 50 MB) do not
  fit in cache, so all sizes are bound by DRAM latency and TLB misses.

**n = 1,000,000**

| Operation | sflat | emhash8 | boost_flat | boost_node | absl_flat | absl_node | std |
|---|---|---|---|---|---|---|---|
| insert (reserved) | **67.4** | 94.5 | 92.1 | 275.2 | 105.7 | 144.3 | 382.3 |
| find hit (insert order) | 44.1 | 44.6 | 53.8 | 67.7 | 50.5 | **42.5** | 107.3 |
| find hit (random order) | **45.2** | 82.1 | 55.2 | 107.0 | 50.3 | 97.5 | 144.3 |
| find miss | 32.9 | 34.7 | **14.2** | 81.3 | 41.4 | 44.9 | 161.5 |
| erase | **70.6** | 160.2 | 92.6 | 258.0 | 87.2 | 219.7 | 453.7 |
| iterate | 22.3 | **1.9** | 20.1 | 57.0 | 17.8 | 47.0 | 70.0 |
| insert (no reserve) | 141.5 | 232.3 | **131.5** | 544.5 | 138.8 | 325.5 | 584.1 |
| bytes/entry (steady) | **21.6** | 34.1 | 34.8 | 46.6 | 36.9 | 52.2 | 41.8 |
| bytes/entry (growth peak) | 40.7 | **36.2** | 51.3 | 46.7 | 54.4 | 58.4 | 45.1 |

At 1M the no-reserve table lands just after a 2x step (2.1M slots, load
0.48). This is the reason for the 40.7 bytes/entry growth peak. It is the
cost of 2x growth for small tables.

**n = 10,000,000**

| Operation | sflat | emhash8 | boost_flat | boost_node | absl_flat | absl_node | std |
|---|---|---|---|---|---|---|---|
| insert (reserved) | 155.0 | **133.6** | 234.9 | 370.2 | 203.6 | 245.9 | 481.5 |
| find hit (insert order) | 66.9 | **64.1** | 111.2 | 106.0 | 107.7 | 81.5 | 167.1 |
| find hit (random order) | **67.8** | 136.8 | 111.1 | 193.6 | 107.1 | 206.0 | 245.9 |
| find miss | 52.5 | 54.2 | **40.4** | 150.1 | 101.9 | 98.2 | 240.9 |
| erase | **159.9** | 212.6 | 183.2 | 366.8 | 186.3 | 287.3 | 603.0 |
| iterate | 22.2 | **2.0** | 18.3 | 86.8 | 16.5 | 65.4 | 95.5 |
| insert (no reserve) | **181.8** | 299.0 | 224.2 | 664.1 | 215.1 | 471.8 | 789.7 |
| bytes/entry (steady) | **19.9** | 29.6 | 27.0 | 42.8 | 28.7 | 47.2 | 40.4 |
| bytes/entry (growth peak) | 29.9 | **29.6** | 40.4 | 42.8 | 42.9 | 47.2 | 41.8 |

**n = 50,000,000**

| Operation | sflat | emhash8 | boost_flat | boost_node | absl_flat | absl_node | std |
|---|---|---|---|---|---|---|---|
| insert (reserved) | **181.1** | 234.3 | 324.5 | 489.5 | 234.9 | 288.3 | 567.9 |
| find hit (insert order) | **71.8** | 105.1 | 196.7 | 182.0 | 194.1 | 155.0 | 260.1 |
| find hit (random order) | **71.6** | 274.4 | 200.3 | 353.9 | 194.4 | 347.2 | 428.1 |
| find miss | 60.4 | 99.3 | **57.9** | 272.6 | 190.0 | 186.1 | 349.3 |
| erase | **179.0** | 285.2 | 221.4 | 492.4 | 225.0 | 370.1 | 748.0 |
| iterate | 22.2 | **2.0** | 16.7 | 160.2 | 15.5 | 140.6 | 117.7 |
| insert (no reserve) | **207.9** | 407.7 | 315.9 | 767.2 | 258.1 | 509.9 | 1341.0 |
| bytes/entry (steady) | **19.6** | 26.8 | 21.5 | 40.6 | 22.8 | 44.1 | 40.7 |
| bytes/entry (growth peak) | **23.7** | 26.7 | 32.2 | 40.6 | 34.2 | 44.1 | 56.2 |

50M: 1 run.

#### THP on and off (sflat only)

sflat is the only map here that asks for huge pages. To separate the effect
of THP from the effect of the map design, the same benchmark was run with
`-DSFLAT_USE_THP=0` (sflat then uses 4 KiB pages like the other maps).
Median of 3 runs at 1M and 10M, 1 run at 50M. The best of the other flat
maps (boost_flat or absl_flat, from the THP-off run) is shown for reference.

| ns/op | 1M THP off | 1M THP on | 10M THP off | 10M THP on | 50M THP off | 50M THP on | best other flat, 50M |
|-------|-----------|-----------|-------------|------------|-------------|------------|----------------------|
| insert (reserved) | 81.1 | **67.4** | 219.0 | **155.0** | 325.2 | **181.1** | 234.5 (absl_flat) |
| find hit (random order) | 52.6 | **45.2** | 121.1 | **67.8** | 209.8 | **71.6** | 194.7 (absl_flat) |
| insert (no reserve) | 160.1 | **141.5** | 246.1 | **181.8** | 332.5 | **207.9** | 255.2 (absl_flat) |

On this machine THP is better in every row and at every size. The gain
grows with the table: at 50M a random find is 2.9 times faster with THP.
The Tremont core has a small TLB, so with 4 KiB pages nearly every random
access to a large table also misses the TLB and walks the page tables.

With THP off, sflat is a little slower than boost_flat and absl_flat (7 to
14 percent on find hit at 10M and 50M, and also slower on insert). Most of the
lead of sflat in the 10M and 50M tables above therefore comes from huge
pages, not from the probing scheme. Other maps can get a similar gain with
an allocator that uses huge pages, or with THP set to `always`.

Unlike on the Xeon VM (where the fault cost of huge pages made small tables
slower), there is no THP penalty on this bare-metal machine, even at 1M.

#### Honest assessment

- **memory (steady state)**: sflat uses the least memory at all sizes: 9 to
  38 percent less than boost_flat, 14 to 41 percent less than absl_flat, and
  27 to 37 percent less than emhash8.
- **memory (growth peak)**: at 50M sflat has the lowest peak (23.7
  bytes/entry). At 10M it is equal to emhash8 (29.9 and 29.6). At 1M the 2x
  step makes it 40.7, more than emhash8 (36.2).
- **find hit (random order)**: sflat is the fastest at all sizes (45, 68
  and 72 ns). At 10M and 50M this is mainly the THP effect (see above).
  With THP off it is 7 to 14 percent slower than absl_flat.
- **insert**: sflat is the fastest with reserve at 1M and 50M, and without
  reserve at 10M and 50M. emhash8 is faster with reserve at 10M, and
  boost_flat without reserve at 1M. With THP off, absl_flat is faster on
  insert at 10M and 50M.
- **find miss**: boost_flat is still the fastest at all sizes (2.3 times
  faster at 1M, about equal at 50M). sflat fills the table to 0.875, so the
  overflow bits are often set.
- **erase**: sflat is the fastest at all sizes here.
- **iterate**: not a goal. sflat is up to 1.4 times slower than boost_flat
  and absl_flat.
- **SSE2 only**: this CPU has no AVX2, so sflat probes 16-slot groups. The
  results show that the SSE2 path is competitive.

### Apple M2, MacBook Air (macOS)

#### Test environment

| Item | Value |
|------|-------|
| Machine | MacBook Air (Mac14,2), fanless, on AC power |
| CPU | Apple M2, 8 cores (4 performance, 4 efficiency) |
| ISA | ARMv8.6-A, NEON (sflat uses the NEON x16 back end) |
| Cache | P cores: L1d 128 KiB per core, L2 16 MiB shared. E cores: L1d 64 KiB, L2 4 MiB. 128-byte cache lines |
| Memory | 16 GiB LPDDR5 (unified), 16 KiB pages |
| OS | macOS 26.6.2 (25G83) |
| Compiler | Apple clang 21.0.0, libc++, `-O3 -mcpu=native -std=c++17 -DNDEBUG` |

- Comparison targets: emhash8 (upstream commit 801d02a),
  boost::unordered_flat_map and boost::unordered_map (Boost 1.86.0),
  absl::flat_hash_map and absl::node_hash_map (Abseil 20260107.1 from
  Homebrew), std::unordered_map (libc++). All use the macOS system malloc.
- Same program and method as on the Xeon (`bench/bench.cpp`). On macOS
  `ru_maxrss` is in bytes; the benchmark converts it.
- 1M and 10M: median of 3 runs. 100M: 1 run.
- The process is not pinned to a core type. macOS runs it on the
  performance cores while they are free.

Build command (Abseil from Homebrew):

```bash
A=$(brew --prefix abseil)
clang++ -std=c++17 -O3 -mcpu=native -DNDEBUG -DHAS_ABSL \
  -I include -I thirdparty/emhash/include -I thirdparty/boost/include \
  -I $A/include bench/bench.cpp -L$A/lib \
  -labsl_hash -labsl_city -labsl_raw_hash_set -labsl_hashtablez_sampler \
  -o bench_m2
./bench_m2 1000000 10000000 100000000
```

#### Results

Lower is better. **Bold** is the best value in the row. The rows mean the
same as in the Xeon results.

**n = 1,000,000**

| Operation | sflat | emhash8 | boost_flat | boost_node | absl_flat | absl_node | std |
|---|---|---|---|---|---|---|---|
| insert (reserved) | 10.9 | 20.2 | **7.7** | 35.7 | 16.3 | 26.0 | 43.9 |
| find hit (insert order) | 7.3 | **4.9** | 7.0 | 13.1 | 6.8 | 6.1 | 15.2 |
| find hit (random order) | **5.3** | 9.3 | 6.3 | 18.4 | 6.0 | 10.6 | 21.3 |
| find miss | 12.4 | 7.5 | **2.4** | 16.9 | 3.2 | 3.9 | 31.4 |
| erase | **9.0** | 11.1 | 9.3 | 47.6 | 14.7 | 28.9 | 89.8 |
| iterate | 7.9 | **0.2** | 8.2 | 10.3 | 6.7 | 7.7 | 8.8 |
| insert (no reserve) | 24.6 | 41.3 | **20.0** | 62.1 | 21.1 | 33.9 | 85.9 |
| bytes/entry (steady) | **19.6** | 32.9 | 33.6 | 45.6 | 35.8 | 35.1 | 40.2 |
| bytes/entry (growth peak) | 67.4 | 75.4 | 67.2 | 58.3 | 71.6 | **54.2** | 58.7 |

**n = 10,000,000**

| Operation | sflat | emhash8 | boost_flat | boost_node | absl_flat | absl_node | std |
|---|---|---|---|---|---|---|---|
| insert (reserved) | 15.8 | 36.0 | **12.1** | 69.8 | 18.5 | 32.7 | 90.3 |
| find hit (insert order) | 14.2 | 11.4 | 11.6 | 23.5 | **9.4** | 10.2 | 24.5 |
| find hit (random order) | 14.4 | 15.2 | 11.9 | 29.7 | **9.5** | 17.7 | 32.2 |
| find miss | 14.5 | 11.1 | **4.8** | 26.7 | 7.0 | 9.1 | 47.7 |
| erase | 24.0 | 27.1 | **17.0** | 94.5 | 25.1 | 57.3 | 164.2 |
| iterate | 7.8 | **0.3** | 7.8 | 13.2 | 6.5 | 7.6 | 17.3 |
| insert (no reserve) | 26.6 | 63.3 | **20.6** | 107.2 | 23.8 | 48.6 | 185.7 |
| bytes/entry (steady) | **19.5** | 29.4 | 26.8 | 42.8 | 28.5 | 31.2 | 40.1 |
| bytes/entry (growth peak) | 53.7 | 64.2 | 53.7 | 52.4 | 57.1 | **46.3** | 53.2 |

**n = 100,000,000**

| Operation | sflat | emhash8 | boost_flat | boost_node | absl_flat | absl_node | std |
|---|---|---|---|---|---|---|---|
| insert (reserved) | 32.1 | 43.6 | **23.5** | 83.5 | 29.5 | 54.1 | 98.3 |
| find hit (insert order) | 17.3 | 14.0 | 14.9 | 27.6 | **12.8** | 13.9 | 27.0 |
| find hit (random order) | 18.0 | 17.5 | 15.1 | 34.1 | **13.2** | 20.4 | 34.0 |
| find miss | 24.3 | 12.6 | **10.6** | 32.0 | 15.8 | 17.0 | 50.2 |
| erase | 32.7 | 32.3 | **21.7** | 104.8 | 34.2 | 76.5 | 176.3 |
| iterate | 7.8 | **0.3** | 7.4 | 14.6 | 6.4 | 7.1 | 23.5 |
| insert (no reserve) | 40.2 | 86.5 | **29.7** | 124.4 | 34.1 | 68.9 | 205.9 |
| bytes/entry (steady) | **19.5** | 26.7 | 21.5 | 40.7 | 22.8 | 28.1 | 40.1 |
| bytes/entry (growth peak) | 43.0 | 44.3 | 43.0 | 46.3 | 45.6 | **40.2** | 47.3 |

About the growth peak on macOS: the macOS malloc keeps large freed blocks
mapped and dirty in the process for reuse. So the old tables of a growing
map stay in the peak RSS (and in the physical footprint, which was checked
too) after the map frees them. This makes the growth peak about 2x the final
table for every map. sflat has no mmap path on macOS (see "Growth memory"),
so its page release does not help here. The steady-state row is not
affected.

#### Why find is slower on the M2, and the find tuning

Measured with small test programs on this machine (10M entries, hits in
random order):

1. **One find is not slower.** With each key depending on the previous
   result (no overlap between finds), a find costs 121 ns in sflat, 123 ns
   in boost_flat and 112 ns in absl_flat. The difference is how many finds
   run at the same time (latency / time per find): about 7.8 for sflat
   (before the tuning below), 10.4 for boost_flat and 12.0 for absl_flat.
2. **The memory pattern is not the limit.** The same accesses as sflat
   (control bytes, then the slot) in a bare loop with sflat's array sizes
   take about 8 ns per lookup. The 10 MB control array mostly hits in the
   16 MiB L2 cache, so a hit has one DRAM miss.
3. **The limit is instructions per find.** A DRAM miss takes about 110 ns,
   so many finds must be in flight at once, and the out-of-order window
   holds only so many instructions. In the bare loop, every extra
   instruction per lookup adds about 0.4 ns (+10, +20, +30, +40
   instructions: 10.6, 13.2, 17.3, 21.4, 25.9 ns). The hit path of the find
   loop had about 42 instructions in sflat, 33 in boost_flat and 30 in
   absl_flat. At 0.4 ns per instruction this predicts the measured gaps.

The tuning therefore removes instructions from the hit path:

- **Small inlined part, out-of-line rest.** `find()` inlines only the home
  group: a hit there, or a miss that the home group proves. Probing past
  the home group and the old table of a migration go to one `noinline`
  function. It is marked `pure` (it writes no memory), so callers keep the
  table fields in registers across the call. It takes small trivially
  copyable keys by value, so callers do not store the key to the stack.
  (Forcing the whole find inline was worse: Clang then refused to inline
  small wrappers around `find()`, such as the benchmark's.)
- **Lane offset in one shift.** The NEON mask keeps the low bit of each
  4-bit lane, so `ctz` is a multiple of 4, and the compiler is told so.
  The key compare comes before `m &= m - 1`.
- **Slot prefetch** (`SFLAT_PREFETCH_SLOTS=1`, NEON only): `find()`
  prefetches the home group's slots (two 128-byte lines) before it reads
  the control bytes, so the two misses overlap. One line was slower: half
  of the hits then miss again in the second line.
- Tried without gain: computing H2 with a vector `umin`, telling the
  compiler that a hit pointer is not null.

Effect in the benchmark above (sflat, ns/op, before and after the tuning):

| Operation | 1M | 10M | 100M |
|---|---|---|---|
| find hit (random order) | 6.3 to 5.3 | 18.4 to 14.4 | 22.3 to 18.0 |
| find hit (insert order) | 8.4 to 7.3 | 18.0 to 14.2 | 21.2 to 17.3 |
| find miss | 13.5 to 12.4 | 15.8 to 14.5 | 27.1 to 24.3 |
| erase | 9.8 to 9.0 | 27.3 to 24.0 | 35.3 to 32.7 |

The split of `find()` and the lane order are plain C++ and apply to all
back ends; they were not measured on x86-64. The prefetch, the forced
inlining of the small part and the NEON lane offset are NEON only.

#### NEON compared with SWAR

sflat only, ns/op, median of 3 runs, both before the find tuning above.
The NEON compare is not faster than the portable SWAR group (8 bytes in a
`uint64_t`): on the M2 a find waits for memory, not for the compare. SWAR
is even faster for misses.

| Operation | n | SWAR x8 | NEON x16 |
|---|---|---|---|
| insert (reserved) | 1M | 13.0 | **12.4** |
| find hit (random order) | 1M | **7.3** | 8.5 |
| find miss | 1M | **11.0** | 14.4 |
| erase | 1M | **12.0** | 12.9 |
| insert (reserved) | 10M | 16.7 | **16.3** |
| find hit (random order) | 10M | **23.5** | 24.5 |
| find miss | 10M | **13.1** | 15.6 |
| erase | 10M | **32.2** | 33.4 |

#### Assessment (M2)

- **memory (steady state)**: sflat uses the least memory at all sizes
  (19.5 to 19.6 bytes/entry): 9 to 42 percent less than boost_flat, 14 to
  45 percent less than absl_flat, and 27 to 40 percent less than emhash8.
- **memory (growth peak)**: equal to boost_flat at all sizes. All maps
  pay for the macOS malloc cache (see above).
- **find hit (random order)**: sflat is the fastest at 1M (5.3 ns). At 10M
  and 100M it is 1.2 times slower than boost_flat and 1.4 to 1.5 times
  slower than absl_flat (before the tuning: 1.5 to 2.0 times). The x86-64
  machines differ mainly because of THP: sflat uses 2 MiB pages on Linux,
  and there was its lead at large sizes (Xeon, 100M: 43.0 ns with THP,
  51.6 without, boost_flat 44.3). macOS has no such path.
- **insert**: not tuned yet. boost_flat is the fastest. sflat is second
  or third for reserved inserts and 1.2 to 1.4 times slower than
  boost_flat without reserve.
- **find miss**: boost_flat is 2.3 to 5.2 times faster, for the same
  reason as on the x86-64 machines (load factor 0.875, so the overflow bits
  are often set).
- **erase**: sflat is the fastest at 1M; at 10M and 100M boost_flat is
  1.4 to 1.5 times faster.

### Large-scale estimates (not measured)

The table below extrapolates from the measured steady-state bytes/entry.

| Entries | sflat (about 20 B/entry) | boost_flat (about 30 B/entry) |
|---------|--------------------------|--------------------------|
| 100M    | about 2.0 GB             | about 3.0 GB             |
| 1B      | about 20 GB              | about 30 GB              |
| 100B    | about 2.0 TB             | about 3.0 TB             |

With page release, the growth peak is about the size of the new table
(2x, 1.5x or 1.25x of the old table) plus the control bytes. The measured
peak is 23.6 bytes/entry at 100M (Xeon) and 23.7 at 50M (Celeron). The actual values depend on the allocator and on the key
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

This project uses the GNU General Public License v3.0. The LICENSE file
has the full text.

The license covers `include/`, `tests/`, and `bench/`. `thirdparty/` holds
third-party code. Each third-party item keeps its original license (emhash,
Boost).
