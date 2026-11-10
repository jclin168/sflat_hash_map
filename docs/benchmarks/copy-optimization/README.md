# Copy benchmark improvement

## Result

The original Copy case is much slower in sflat because a compact copy soon starts growth. Each later copy must hash and insert all live entries. This change gives compact copies a small amount of free capacity. It also lets copy assignment reuse compatible storage.

This report uses the original map_benchmark Copy case. It starts with 1,000,000 random `uint64_t` key/value pairs. Each measured loop makes 200 copies. The loop adds one entry to the source after each copy. Times below are medians of five fresh processes, in seconds per 200 copies. All result checks passed.

| Container | Copy constructor | Copy assignment | Sum of medians |
|---|---:|---:|---:|
| sflat before | 3.602040 | 3.940710 | 7.542750 |
| sflat after | 0.505530 | 0.353378 | 0.858908 |
| Ankerl unordered_dense 5.3.3 | 0.527303 | 1.462680 | 1.989983 |

The constructor is 7.13 times as fast as before. Assignment is 11.15 times as fast. The constructor is close to Ankerl in this run. Assignment is 4.14 times as fast as Ankerl. These are results for this workload and machine. They do not establish a new overall rank across all benchmark cases.

The median process peak RSS at the end of Copy was 101.6 MiB before and 75.7 MiB after. Ankerl used 100.1 MiB. RSS includes the input map, source copies, destination, allocator, and executable. It is not the size of one table. The CSV preserves the reported current RSS, peak RSS, and baseline metrics.

## Cause

1. The input map has 2,097,152 slots. The first compact copy originally has 1,142,880 slots at a maximum load factor of 0.875.
2. That copy can accept only 20 more entries before growth. At loop index 21, its capacity grows to 2,285,760 slots and incremental migration starts.
3. A copy of a source that is in migration uses the insertion path. The remaining 179 copies make a total of 179,019,690 hash calls. Inserts between copies advance migration too slowly to finish it.
4. Copy assignment also creates a temporary table and then swaps it with the destination. It cannot reuse the destination allocation.

Ankerl copies its dense values and index without rebuilding the hash table. It therefore avoids the repeated hashing in this case.

The counting-hash diagnostic is separate from the timed runs. See `diagnose.cpp` and its before/after output. Its initial compact copy still hashes all 1,000,000 entries. After this change, its next 200 copies make zero hash calls.

## Change and memory limit

- Round compact copy capacity up to a block of 4096 slots, only if the result stays within the existing `SFLAT_COPY_SLACK` limit. The default limit stays 1.25. Values at or below 1 keep exact minimum capacity. A value of 0 also disables direct table cloning.
- At 1M entries, capacity rises from 1,142,880 to 1,146,880 slots: 4000 slots, or 0.35%. With 16-byte values, the extra logical value storage is 64,000 bytes, plus control and overflow metadata. Linux already rounds both value allocations to the same 18 MiB mapping. This small capacity increase prevents a much larger migrating table in this workload.
- Reuse copy-assignment storage only for trivial values with the default allocator, equal capacity, stable tables, an eligible source, and hash/equality objects with nonthrowing copy assignment. Copy all hash state and metadata. Keep copy-and-swap for other cases.
- Apply the same policy to map and set. Do not change table fields, insert code, find code, growth thresholds, or the default load factor.

Rounding does not guarantee free capacity for every size. A minimum capacity that is already block-aligned gets no extra slots. Small tables and tables that cannot fit the rounding within the slack bound retain their old capacity.

## Insert and find checks

These checks use the controlled FlatMap64 cases with fixed input and std::hash. Each cell is a median of 11 fresh processes. Times are milliseconds. Negative change means less time.

| Entries | Operation | Before, ms | After, ms | Time change |
|---|---|---:|---:|---:|
| 1,000,000 | reserve+insert | 16.534 | 15.659 | -5.29% |
| 1,000,000 | find_hit_random | 11.458 | 11.354 | -0.91% |
| 1,000,000 | find_miss | 7.238 | 7.465 | +3.13% |
| 1,000,000 | insert_grow | 55.929 | 51.051 | -8.72% |
| 10,000,000 | reserve+insert | 243.265 | 263.871 | +8.47% |
| 10,000,000 | find_hit_random | 262.215 | 259.104 | -1.19% |
| 10,000,000 | find_miss | 162.313 | 163.447 | +0.70% |
| 10,000,000 | insert_grow | 513.877 | 517.561 | +0.72% |

The full benchmark executable shows an 8.47% slower median for 10M reserve+insert. Keep this observed difference in the assessment. The other operations do not show a consistent slowdown. The shared machine also produced outliers; the raw data preserves them.

To check whether the insert/find code changed, compile a translation unit with only getRSS.cpp, main.cpp, and FlatMap64.cpp. With the same flags, the before and after executables are byte-for-byte identical. Both have SHA-256 `d52edbf129ba12635851ad10fb1cf7b9f6c7543413852e9370096c7fc85700d4`. This rules out a change to the generated insert/find code in that isolated build. The full executable has a different code layout after the copy changes. These results do not prove a universal zero cost for every compiler or application.

## Validation

- All nine CMake correctness executables passed, including the new map/set copy tests with normal and forced incremental growth.
- New tests cover storage reuse, hash/equality state, load factor, tombstones, self-assignment, empty sources, source independence, migration, and exception cleanup.
- Copy tests passed with `SFLAT_COPY_SLACK=0`, `SFLAT_COPY_SLACK=2`, and SSE2 with mmap disabled.
- AddressSanitizer and UndefinedBehaviorSanitizer passed with mmap disabled and forced incremental growth. LeakSanitizer cannot run under this environment's tracing setup; that run used `ASAN_OPTIONS=detect_leaks=0`. The throwing-value test also checks live object counts.

## Environment and reproduction

Intel Xeon Platinum 8573C; Linux x86-64; GCC 14.2; C++17; `-O3 -DNDEBUG -march=native`; `SFLAT_PROCESS_SEED=1`. Processes run sequentially on CPU 0. The cgroup allows two CPU cores and 8 GiB RAM. No compilation or correctness tests run concurrently with the timed measurements.

The baseline header is from sflat `f163c3d`. Its bytes match the header on current main `c41f3f8`, which is the parent of this change. Ankerl is `7c58a8b` (5.3.3). Full commits, source hashes, and binary hashes are in the JSON files. The map_benchmark source uses the integration and memory metrics published in [the comprehensive report](https://github.com/jclin168/map_benchmark/blob/master/docs/results/2026-11-10-stdhash/REPORT.md). Existing archived results remain unchanged.

Build the same map_benchmark sflat target twice. Use the baseline include directory for the first build, then this change's include directory for the second. Keep all other sources and flags equal. Build Ankerl with the same release flags. Run:

```sh
python3 run.py --before /path/to/bench_sflat_before \
  --after /path/to/bench_sflat_after --ankerl /path/to/bench_ankerl \
  --output /path/to/results --cpu 0 --regression-repeats 11
```

The recorded run first used three regression repeats, then eight more repeats. The CSV marks those two phases. The reproduction script uses one batch of 11 repeats. Copy uses five repeats in both cases. Each process enables `MAP_BENCH_EXTRA_MEMORY=1`. FlatMap64 uses `MAP_BENCH_SEED=42` and `MAP_BENCH_N` of 1M or 10M.

Build `diagnose.cpp` with the same sflat flags and the map_benchmark `src/app` include directory for `sfc64.h`. Run it once with each header version. Its counter adds overhead, so do not use its runtime as a performance result.
