# sflat::flat_hash_map

sflat::flat_hash_map is a C++17 header-only hash map. It uses open
addressing. The interface matches the common subset of std::unordered_map.

The design has three goals. First, insert and find are fast. Second, the
growth factor is 1.5x, not 2x. This uses less memory. Third, incremental
rehash avoids a latency spike during growth. The map suits large data sets
where steady-state memory is important.

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
- The growth factor is about **1.5x**. The measured maximum is 1.667x. This
  occurs only in very small tables:
  ```
  64 -> 96 -> 160 -> 256 -> 384 -> 608 -> ...
  ```
- The default max load factor is 0.875.
- **Incremental rehash**: the map does not move all items at once during
  growth. Each operation moves 64 slots to the new table. This avoids a
  latency spike. It does **not** decrease the transient memory peak: the old
  table (1x) and the new table (1.5x) are both alive until the migration
  ends. The measured growth peak is in the benchmark below. During migration,
  `find` checks the new table first and then the old table. `insert` writes
  only to the new table. `begin()` completes the migration first, because
  iteration is not a hot path.

Memory formula for `uint64_t -> uint64_t`:
```
bytes/entry = (16 + 1 + 1/32) / 0.875 = about 19.5
```
The measured value is 19.5 to 20.7 bytes/entry. The difference is allocator
overhead.

### Why 1.5x uses less memory than 2x

A 2x growth needs a 3x peak during rehash (old table plus new table). The
steady-state capacity can reach 2x of the need. A 1.5x growth reduces these
to about 2.5x and 1.5x. For 100M entries at 16 bytes each:

| Strategy | Steady-state capacity | Rehash peak |
|----------|----------------------|-------------|
| 2x       | about 3.2 GB         | about 4.8 GB |
| 1.5x     | about 2.1 GB         | about 3.5 GB |

These are theoretical estimates. They are the worst case for each strategy.
The actual peak also depends on where `n` falls relative to the growth
steps. See the note below.

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
- During a migration, some `const` functions (`find`, `count`, `contains`,
  `at`, `begin`, `cbegin`) change internal state. They move items to the
  new table. Thus concurrent reads from many threads are not safe while a
  migration is in progress. Call `finish_migration()` (or `begin()`) from
  one thread before you share the map for reads.
- The `overflow_` array needs fewer than 2^32 groups. This is 137 billion
  slots. In practice the map never reaches this limit.

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

Measured speedup: 1.04x at 100M entries (80.8 ns to 77.9 ns per op). No
gain at 10M. The CPU already overlaps misses well in a simple loop. The
benefit grows with table size.

## Benchmark

### Test environment

- CPU: Intel Xeon @ 2.10 GHz (4 vCPU, AVX2 and AVX-512), RAM 15 GiB.
- Compiler: g++ 13.3.0 with `-O3 -march=native -std=c++17`.
- Comparison targets: emhash8 (upstream commit 801d02a, load factor 0.80),
  boost::unordered_flat_map and boost::unordered_map (Boost 1.83),
  std::unordered_map (libstdc++ 13).
- Test: `uint64_t -> uint64_t` with random keys. Each map runs in its own
  child process. Memory is the child peak RSS minus the baseline (key and
  value arrays only).
- One run per size. Expect about 10 percent noise between runs.

### Results

Lower ns/op is better. Lower bytes/entry is better. **Bold** is the best
value in the row.

"insert (reserved)" includes the `reserve(n)` call. "growth peak" is the
peak RSS of the no-reserve test. It includes the time when the old and the
new table are both alive.

**n = 1,000,000**

| Operation | sflat | emhash8 | boost_flat | boost_node | std |
|-----------|-------|---------|------------|------------|-----|
| insert (reserved) | 34.6 | 35.6 | **25.7** | 81.6 | 100.3 |
| find (hit) | 20.3 | 9.4 | **9.2** | 18.6 | 24.7 |
| find (miss) | 17.1 | 13.0 | **5.2** | 20.9 | 34.1 |
| erase | 20.8 | 22.5 | **11.6** | 46.4 | 66.8 |
| iterate | 10.8 | **0.6** | 11.0 | 14.2 | 24.0 |
| insert (no reserve) | 68.0 | 86.2 | **49.8** | 136.4 | 164.8 |
| bytes/entry (steady) | **20.7** | 34.1 | 34.8 | 46.7 | 41.9 |
| bytes/entry (growth peak) | 39.1 | **36.1** | 51.1 | 46.7 | 45.2 |

**n = 10,000,000**

| Operation | sflat | emhash8 | boost_flat | boost_node | std |
|-----------|-------|---------|------------|------------|-----|
| insert (reserved) | 107.9 | 92.8 | **35.9** | 224.1 | 245.8 |
| find (hit) | 36.7 | **20.6** | 27.0 | 39.6 | 48.6 |
| find (miss) | 27.8 | 21.4 | **11.4** | 50.2 | 66.2 |
| erase | 50.6 | 52.7 | **37.3** | 110.2 | 189.0 |
| iterate | 12.4 | **1.7** | 12.7 | 30.6 | 76.2 |
| insert (no reserve) | 211.9 | 122.8 | **92.3** | 342.5 | 452.4 |
| bytes/entry (steady) | **19.6** | 29.5 | 27.0 | 42.8 | 40.4 |
| bytes/entry (growth peak) | 42.5 | **29.6** | 40.3 | 42.8 | 41.9 |

**n = 100,000,000**

| Operation | sflat | emhash8 | boost_flat | boost_node | std |
|-----------|-------|---------|------------|------------|-----|
| insert (reserved) | 138.2 | 120.1 | **55.3** | 318.2 | 353.8 |
| find (hit) | 71.7 | **35.0** | 43.2 | 56.3 | 66.5 |
| find (miss) | 43.2 | 34.8 | **22.4** | 75.4 | 81.4 |
| erase | 92.8 | 79.1 | **54.2** | 158.1 | 265.9 |
| iterate | 11.8 | **1.8** | 11.8 | 43.0 | 108.2 |
| insert (no reserve) | 236.5 | 167.0 | **108.5** | 432.2 | 611.6 |
| bytes/entry (steady) | **19.5** | 26.7 | 21.5 | 40.6 | 40.1 |
| bytes/entry (growth peak) | 48.7 | **26.7** | 32.2 | 40.6 | 40.1 |

### Honest assessment

- **memory (steady state)**: this is the main strength of sflat. It uses
  the least memory at all sizes: 9 to 41 percent less than boost_flat, 27
  to 39 percent less than emhash8, and about half of the node-based maps.
- **memory (growth peak)**: sflat has the **highest** peak at 10M and 100M.
  Incremental rehash keeps the old and the new table alive together for
  many operations. Thus it does not decrease the peak. If the peak is
  important, call `reserve(n)` first.
- **insert (reserved)**: boost_flat is 1.3x to 3x faster. After
  `reserve(n)`, sflat fills the table to its 0.875 load limit. boost_flat
  and emhash8 round the capacity up to a power of two. Their final load is
  0.6 to 0.8. Thus sflat probes longer chains. This is the cost of
  the smaller memory.
- **find (hit)**: sflat is 1.8x to 2.2x slower than the best (emhash8 or
  boost_flat). It is faster than std::unordered_map at 1M and 10M.
- **find (miss)**: sflat is 1.9x to 3.3x slower than boost_flat. The same
  high load causes this. Many home groups are full, so the overflow bits
  are often set.
- **erase**: within 1.8x of boost_flat. Faster than the node-based maps.
- **iterate**: equal to boost_flat. emhash8 is much faster because it keeps
  the values in a dense array.
- **flat versus node maps**: all three flat maps are faster than
  boost::unordered_map and std::unordered_map in most operations. The only
  exceptions are find (hit) at 100M and at 1M, where the node maps are
  equal to or slightly faster than sflat.

### Large-scale estimates (not measured)

The table below extrapolates from the measured steady-state bytes/entry.

| Entries | sflat (about 20 B/entry) | boost (about 30 B/entry) |
|---------|--------------------------|--------------------------|
| 100M    | about 2.0 GB             | about 3.0 GB             |
| 1B      | about 20 GB              | about 30 GB              |
| 100B    | about 2.0 TB             | about 3.0 TB             |

Also, 1.5x growth has a peak of about 2.5x of steady state during rehash.
2x growth has a peak of about 3x. The 1.5x strategy is kinder to
memory-limited large tables. These numbers are theoretical. The actual
values depend on the allocator and on the key and value sizes.

## Correctness tests

Run these commands:

```bash
g++ -std=c++17 -O2 -march=native -I include tests/correctness.cpp -o /tmp/correctness
/tmp/correctness
```

The tests include: randomized mixed operations against
`std::unordered_map`, string keys, sequential keys, copy and move, swap,
iterator erase, reserve and rehash, and growth factor checks. They also pass
with `-mno-avx2` (SSE2 path) and with ASan and UBSan.

## License

This project uses the GNU General Public License v3.0. The LICENSE file
has the full text.

The license covers `include/`, `tests/`, and `bench/`. `thirdparty/` holds
third-party code. Each third-party item keeps its original license (emhash,
Boost).
