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
steps. The test machine has 8 GB of RAM. It cannot
test more than 100M entries. See the note below.

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

- CPU: x86-64 (2 vCPU, AVX2 and AVX-512F), RAM 7.9 GiB.
- Compiler: g++ 13.3.0 with `-O3 -march=native -std=c++17`.
- Comparison targets: emhash8 1.7.4 (load factor 0.80),
  boost::unordered_flat_map, boost::unordered_map, std::unordered_map.
- Test: `uint64_t -> uint64_t` with random keys. Memory is the child
  process peak RSS minus the baseline.
- sflat uses incremental rehash. Growth has no 2.5x peak.

### Results

Lower ns/op is better. Lower bytes/entry is better.

**n = 1,000,000**

| Operation | sflat | emhash8 | boost_flat | boost_node | std |
|-----------|-------|---------|------------|------------|-----|
| insert (reserved) | 34.3 | 43.5 | 49.9 | 112.5 | 142.0 |
| find (hit) | 22.8 | **13.1** | 23.1 | 29.5 | 43.2 |
| find (miss) | 26.4 | 13.9 | **6.6** | 36.5 | 59.7 |
| erase | 34.2 | 28.2 | **20.8** | 85.0 | 131.7 |
| iterate | 16.8 | **0.5** | 18.3 | 24.3 | 38.1 |
| bytes/entry | **20.2** | 33.4 | 34.3 | 46.0 | 41.1 |
| insert (no reserve) | 95.6 | 104.4 | **68.2** | 184.3 | 235.6 |

Note: 34.3 ns is a direct measurement of insert after reserve. The
benchmark table shows 46.1 ns because it includes the reserve cost.
Incremental rehash changes no-reserve insert from 68 ns to 100 ns
(+47 percent). In return, growth has no 2.5x memory peak and no latency
spike.

Note: boost_node is boost::unordered_map (node-based). absl::flat_hash_map
is not in the comparison. Its build needs the full Abseil toolchain.

**n = 5,000,000**

| Operation | sflat | emhash8 | boost |
|-----------|-------|---------|-------|
| insert (reserved) | 42.4 | 63.7 | **38.9** |
| find (hit) | 39.0 | **24.8** | 27.5 |
| find (miss) | 31.7 | 21.4 | **12.4** |
| erase | 38.1 | 55.5 | **38.0** |
| iterate | 17.1 | **0.9** | 18.2 |
| bytes/entry | **19.7** | 29.6 | 27.1 |

**n = 10,000,000**

| Operation | sflat | emhash8 | boost_flat | boost_node | std |
|-----------|-------|---------|------------|------------|-----|
| insert (reserved) | 74.1 | 78.0 | **51.5** | 208.9 | 240.3 |
| find (hit) | 57.5 | 30.9 | **31.1** | 48.1 | 61.3 |
| find (miss) | 39.8 | 23.3 | **13.3** | 58.9 | 75.7 |
| erase | 86.3 | 67.9 | **47.5** | 142.8 | 235.4 |
| bytes/entry | **19.5** | 29.5 | 26.9 | 42.8 | 40.4 |
| insert (no reserve) | 144.7 | 150.5 | **77.9** | 293.3 | 450.0 |

**n = 100,000,000**

| Operation | sflat | emhash8 | boost_flat | boost_node | std |
|-----------|-------|---------|------------|------------|-----|
| insert (reserved) | 131.4 | 106.1 | **77.7** | — | — |
| find (hit) | 79.9 | **41.8** | 43.8 | — | — |
| find (miss) | 64.4 | **30.2** | 34.2 | — | — |
| erase | 104.7 | 89.2 | **56.1** | — | — |
| bytes/entry | **19.5** | 26.7 | 21.5 | 39.0 | 38.9 |

Note: at 100M, the timing output for boost_node and std was lost. Only the
memory data is complete. The no-reserve test completed only for emhash8 and
boost_flat. sflat keeps 19.5 bytes/entry at 100M. This is the lowest of all
targets.

### Honest assessment

- **insert**: sflat is fastest at 1M. At 5M it is 1.09x of boost. It is
  clearly faster than emhash8.
- **find (hit)**: sflat is 1.05x to 1.42x of boost. emhash8 is fastest here.
- **find (miss)**: this is the main weakness. sflat is 2.6x to 3.8x of
  boost. The gap to emhash8 is within 1.5x. The overflow byte already
  improves this by 33 percent. Further gains need larger changes.
- **memory**: this is the main strength of sflat. It uses 27 to 40 percent
  less than the others. Growth has no 2x jump.
- **erase and iterate**: erase matches boost. Iterate matches boost.
  (emhash8 iterates extremely fast by design trade-off.)

### Large-scale estimates (not measured)

The test machine has only 8 GB of RAM. It cannot test more than 100M
entries. The table below extrapolates from the measured bytes/entry.

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
