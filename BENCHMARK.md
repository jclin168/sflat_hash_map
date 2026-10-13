# sflat::flat_hash_map benchmarks

This file has the benchmark results for all machines. The design of the map
is in the [README](README.md).

The benchmark has been run on four machines:

| Machine | Type | sflat back end | Sizes |
|---|---|---|---|
| [Intel Xeon (Emerald Rapids)](#intel-xeon-emerald-rapids-kvm-virtual-machine) | KVM virtual machine (cloud) | AVX2 | 1M, 10M, 100M |
| [Intel Celeron N5105](#intel-celeron-n5105-bare-metal) | Bare-metal mini PC | SSE2 (no AVX2) | 1M, 10M, 50M |
| [Intel Core i5-3230M](#intel-core-i5-3230m-bare-metal) | Bare-metal laptop | SSE2 (AVX, no AVX2) | 1M, 10M, 100M |
| [Apple M2](#apple-m2-macbook-air-macos) | MacBook Air (macOS) | NEON | 1M, 10M, 100M |

All use the same program (`bench/bench.cpp`). The Xeon, the Celeron and the
i5 use the same versions of the comparison maps; the M2 uses the versions
listed in its section. The Xeon and i5 results are from the current code
(32-bit overflow words, group-wise iteration); the Celeron and M2 were
measured before these changes, so their sflat miss and iterate rows are
slower than the current code would give.

## Summary across machines

sflat at the largest size of each machine, compared with the best other map
in the same row. ns/op, lower is better. On Linux, sflat uses transparent
huge pages (THP); macOS has no such path.

| Row | Xeon, 100M | Celeron, 50M | i5-3230M, 100M | M2, 100M |
|---|---|---|---|---|
| insert (reserved) | **83.6** / 90.3 absl_flat | **181.1** / 234.3 emhash8 | **82.4** / 115.0 emhash8 | 32.1 / **23.5** boost_flat |
| find hit (random order) | **45.8** / 46.7 boost_flat | **71.6** / 194.4 absl_flat | **36.4** / 74.5 boost_flat | 18.0 / **13.2** absl_flat |
| find miss | 34.7 / **28.6** boost_flat | 60.4 / **57.9** boost_flat | 27.2 / **24.9** boost_flat | 24.3 / **10.6** boost_flat |
| insert (no reserve) | **142.8** / 168.3 boost_flat | **207.9** / 258.1 absl_flat | **92.8** / 175.7 boost_flat | 40.2 / **29.7** boost_flat |
| bytes/entry (steady) | **19.6** / 21.5 boost_flat | **19.6** / 21.5 boost_flat | **19.7** / 21.5 boost_flat | **19.5** / 21.5 boost_flat |

- sflat uses the least memory in steady state on every machine.
- On the x86-64 machines with small caches and TLBs (Celeron, i5) sflat
  leads in insert and find hit. Most of that lead comes from huge pages: with
  `-DSFLAT_USE_THP=0` it is close to boost_flat and absl_flat (see the THP
  tables in each section).
- At 100M, sflat leads on the Xeon too: insert, find hit and no-reserve
  insert. The THP-off table in the Xeon section shows how much of this
  comes from huge pages.
- boost_flat is the fastest on find miss everywhere. With the 32-bit
  overflow words the gap is 1.1 to 1.3 times on the i5 (was up to 2.1) and
  1.2 to 1.7 times on the Xeon (was 1.4 to 3.3).
- On the M2 (no THP) boost_flat and absl_flat are faster at 10M and 100M,
  except in memory.

## Running the benchmark

The comparison maps are not in git. Put them in `thirdparty/`:

```bash
mkdir -p thirdparty && cd thirdparty
git clone https://github.com/ktprime/emhash emhash && git -C emhash checkout 801d02a
# Boost headers: thirdparty/boost/include/boost/...   (or the system Boost)
# Abseil (optional): build it and install to thirdparty/abseil
cmake -S abseil-cpp -B abseil-build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_STANDARD=17 -DABSL_PROPAGATE_CXX_STD=ON \
  -DCMAKE_INSTALL_PREFIX=$PWD/abseil
cmake --build abseil-build -j && cmake --install abseil-build
cd ..
```

Build and run (CMake adds `-O3 -march=native -DNDEBUG`, and `-DHAS_ABSL`
when it finds Abseil):

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/bench 1000000 10000000 100000000
```

For the THP-off runs, configure a second build with
`-DCMAKE_CXX_FLAGS=-DSFLAT_USE_THP=0`. To pin the run to one core on Linux,
use `taskset -c <cpu> ./build/bench ...` (the child processes inherit it).

## Intel Xeon (Emerald Rapids), KVM virtual machine

### Test environment

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
- All maps are compiled with `-DNDEBUG`, so no debug asserts run.
- Code: commit b0f6166 (32-bit overflow words, group-wise iteration,
  128-bit MixHash).
- Test: `uint64_t -> uint64_t` with random keys. Each map runs in its own
  child process. Memory is the child peak RSS minus the baseline (the input
  arrays only).
- Before each size, a warm-up child touches and frees memory. In a VM the
  host backs guest memory on first use, which is very slow. Without the
  warm-up, the first map in the run order pays this cost.
- 1M and 10M: median of 3 runs. 100M: 1 run.

### Results

Lower ns/op is better. Lower bytes/entry is better. **Bold** is the best
value in the row.

- "insert (reserved)" includes the `reserve(n)` call.
- "find hit (insert order)" asks for the keys in insertion order. This
  favors emhash8, which stores the elements densely in insertion order: its
  element reads become sequential. "find hit (random order)" is the fair
  test.
- "growth peak" is the peak RSS of the no-reserve test.
- sflat grows 2x below 2 GiB, so all three sizes here use the 2x tier.

The speed of this VM changes from run to run. During this run a first
touch of new memory took 2 to 15 microseconds per 4 KiB page (bare metal:
about 0.25). In an earlier run on the same day all maps were 10 to 30
percent faster at 1M and 10M (for example boost_flat find hit at 1M: 9.5
ns, here 13.7). Compare the maps in the same table, not with other runs.

**n = 1,000,000**

| Operation | sflat | emhash8 | boost_flat | boost_node | absl_flat | absl_node | std |
|---|---|---|---|---|---|---|---|
| insert (reserved) | **32.4** | 45.6 | 43.9 | 137.0 | 49.2 | 96.7 | 169.2 |
| find hit (insert order) | 14.0 | **12.2** | 16.9 | 27.0 | 21.1 | 22.5 | 39.3 |
| find hit (random order) | **12.4** | 17.5 | 13.7 | 36.1 | 20.1 | 30.8 | 41.3 |
| find miss | 8.8 | 14.8 | **6.2** | 32.2 | 8.9 | 12.5 | 46.6 |
| erase | 19.3 | 31.3 | **18.6** | 75.7 | 40.7 | 74.8 | 111.8 |
| iterate | 9.2 | **0.9** | 13.0 | 24.0 | 10.4 | 18.2 | 48.6 |
| insert (no reserve) | **67.2** | 118.0 | 88.5 | 247.1 | 81.6 | 165.6 | 278.6 |
| bytes/entry (steady) | **21.4** | 33.9 | 34.7 | 46.5 | 36.8 | 52.0 | 41.7 |
| bytes/entry (growth peak) | 40.5 | **36.0** | 51.1 | 46.6 | 54.2 | 58.2 | 45.1 |

The sflat no-reserve insert at 1M was 214.3 ns in the first of the three
runs and 64.9 to 67.2 ns in the other two (huge page faults; see "THP on
and off" below). Reserved insert was stable: 30.4 to 35.2 ns.

At 1M the no-reserve table lands just after a 2x step (2.1M slots, load
0.48). This is the reason for the 40.5 bytes/entry growth peak. It is the
cost of 2x growth for small tables.

**n = 10,000,000**

| Operation | sflat | emhash8 | boost_flat | boost_node | absl_flat | absl_node | std |
|---|---|---|---|---|---|---|---|
| insert (reserved) | 55.8 | 87.2 | **52.7** | 343.2 | 68.5 | 230.5 | 289.9 |
| find hit (insert order) | 30.2 | **24.7** | 31.1 | 44.4 | 34.7 | 35.3 | 59.9 |
| find hit (random order) | 33.7 | 37.1 | **32.6** | 64.8 | 35.7 | 59.6 | 79.0 |
| find miss | 24.6 | 25.2 | **14.1** | 57.2 | 17.0 | 23.4 | 88.8 |
| erase | 65.2 | 64.2 | **40.4** | 125.8 | 60.7 | 142.1 | 241.1 |
| iterate | 10.7 | **1.2** | 13.8 | 34.3 | 9.4 | 20.1 | 87.5 |
| insert (no reserve) | **83.1** | 208.6 | 186.9 | 394.2 | 223.1 | 250.2 | 563.3 |
| bytes/entry (steady) | **20.0** | 29.5 | 27.0 | 42.8 | 28.6 | 47.2 | 40.4 |
| bytes/entry (growth peak) | 30.1 | **29.6** | 40.3 | 42.8 | 42.9 | 47.2 | 41.8 |

**n = 100,000,000**

| Operation | sflat | emhash8 | boost_flat | boost_node | absl_flat | absl_node | std |
|---|---|---|---|---|---|---|---|
| insert (reserved) | **83.6** | 182.4 | 91.2 | 472.1 | 90.3 | 372.1 | 510.5 |
| find hit (insert order) | 48.1 | **34.9** | 46.1 | 71.1 | 52.1 | 56.6 | 92.4 |
| find hit (random order) | **45.8** | 57.5 | 46.7 | 99.3 | 52.0 | 90.9 | 107.6 |
| find miss | 34.7 | 36.5 | **28.6** | 103.7 | 38.3 | 54.2 | 137.7 |
| erase | 68.1 | 89.1 | **61.7** | 221.3 | 79.4 | 195.5 | 392.1 |
| iterate | 10.4 | **1.2** | 14.1 | 54.9 | 9.9 | 26.9 | 159.9 |
| insert (no reserve) | **142.8** | 323.8 | 168.3 | 636.3 | 178.5 | 433.2 | 861.8 |
| bytes/entry (steady) | **19.6** | 26.7 | 21.5 | 40.6 | 22.8 | 44.1 | 40.1 |
| bytes/entry (growth peak) | **23.8** | 26.7 | 32.2 | 40.6 | 34.2 | 44.1 | 40.1 |

100M: 1 run.

### Insert latency during growth

Insert 10M random keys with no `reserve`. Each insert is timed with
`rdtsc`. The timing adds overhead to each insert, so the totals are higher
than in the tables above. Values are the range of 3 runs.

| Map | total | p99.9 | p99.99 | max |
|-----|-------|-------|--------|-----|
| sflat | 2.0 to 3.0 s | 2.2 to 2.4 us | 23 to 32 us | **4.8 to 62 ms** |
| boost_flat | 2.0 to 2.2 s | **0.4 to 0.5 us** | **16 to 25 us** | 184 to 192 ms |
| emhash8 | 3.6 to 3.9 s | 2.7 to 3.5 us | 23 to 29 us | 315 to 375 ms |

boost_flat and emhash8 rehash the full table in one insert. sflat spreads
the rehash over many inserts. Its worst case is 3 to 40 times smaller. The
remaining 61 to 62 ms in two of the runs is mostly huge page faults and the
final `MADV_COLLAPSE`; in the third run it was 4.8 ms. (With the older 1.5x
growth rule the sflat max was 30 ms; with 2x growth the last table is
larger.)

### THP on and off (sflat only)

THP cuts TLB misses but makes each first touch of a 2 MiB page slower. This
VM uses free page reporting, so a new huge page often needs host work too.
On bare metal a huge page fault usually costs only the time to clear 2 MiB.
The same runs as above, plus a build with `-DSFLAT_USE_THP=0`. 1M and 10M:
range of 3 runs. 100M: 1 run.

| sflat, ns/op | 1M, THP off | 1M, THP on | 10M, THP off | 10M, THP on | 100M, THP off | 100M, THP on |
|---|---|---|---|---|---|---|
| insert (reserved) | 37 to 51 | **30 to 35** | 59 to 61 | **51 to 59** | 92.7 | **83.6** |
| find hit (random order) | 13 to 14 | 12 to 16 | 36 to 39 | **31 to 34** | 55.9 | **45.8** |
| insert (no reserve) | 79 to 145 | 65 to 214 | **90 to 97** | 82 to 165 | 162.7 | **142.8** |

At 100M, THP is better in all rows: insert 10 percent, find hit 18
percent, no-reserve insert 12 percent. At 10M THP also helps find hit (13
percent). At 1M the table fits in the 260 MiB L3 of this CPU, so the TLB
gain is small, and huge page faults sometimes make no-reserve insert slow.
To turn THP off, define `SFLAT_USE_THP=0`.

### Honest assessment

- **memory (steady state)**: sflat uses the least memory at all sizes: 9 to
  38 percent less than boost_flat, 14 to 42 percent less than absl_flat, and
  27 to 37 percent less than emhash8.
- **memory (growth peak)**: at 100M sflat has the lowest peak (23.8
  bytes/entry). At 10M it is about equal to emhash8 (30.1 and 29.6). At 1M
  the 2x step makes it 40.5, more than emhash8 (36.0).
- **insert (reserved)**: sflat is the fastest at 1M and 100M (32.4 and 83.6
  ns). At 10M boost_flat is 6 percent faster.
- **insert (no reserve)**: sflat is the fastest at all three sizes in this
  run (67.2, 83.1 and 142.8 ns). At 1M boost_flat varied a lot (62.3 to
  104.6 ns over the three runs), so the 1M lead is not clear.
- **find hit (random order)**: sflat is the fastest at 1M and 100M (12.4
  and 45.8 ns). At 10M boost_flat is 3 percent faster.
- **find miss**: boost_flat is 1.2 to 1.7 times faster (was 1.4 to 3.3
  before the 32-bit overflow words). sflat fills the table to 0.875; after
  `reserve(n)` boost_flat is only 51 to 64 percent full at 1M and 10M.
- **erase and iterate**: not a goal. Iterate is faster than boost_flat at
  all sizes; erase is within 1.6x of boost_flat.

## Intel Celeron N5105, bare metal

### Test environment

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

### Results

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

### THP on and off (sflat only)

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

### Honest assessment

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

## Intel Core i5-3230M, bare metal

### Test environment

| Item | Value |
|------|-------|
| Machine | Lenovo laptop (model 20138), on AC power, no virtualization |
| CPU | Intel Core i5-3230M @ 2.60 GHz (3.2 GHz turbo), family 6 model 58 (Ivy Bridge) |
| Cores | 2 cores, 2 threads per core, 1 socket |
| ISA | SSE4.2, AVX, AES-NI. **No AVX2 or BMI2**, so sflat uses the SSE2 16-slot group path |
| Cache | L1d 32 KiB per core, L2 256 KiB per core, L3 3 MiB shared |
| Memory | 15 GiB, 2 GiB swap |
| OS | Ubuntu 26.04.1 LTS |
| Kernel | Linux 7.0.0-38-generic |
| THP | `madvise` (defrag `madvise`) |
| CPU frequency | `intel_cpufreq`, governor `conservative` |
| C library | glibc 2.43 |
| Compiler | g++ 15.2.0, `-O3 -march=native -std=c++17 -DNDEBUG` |

- sflat version: with the 32-bit overflow words and the group-wise
  iteration (see "Profiling on the i5-3230M" below). The other machines
  were measured before these changes.
- Comparison targets: emhash8 (upstream commit 801d02a, load factor 0.80),
  boost::unordered_flat_map and boost::unordered_map (Boost 1.83),
  absl::flat_hash_map and absl::node_hash_map (Abseil LTS 20260817.0),
  std::unordered_map (libstdc++ 15). They use glibc malloc without THP.
- All maps are compiled with `-DNDEBUG`, so no debug asserts run.
- Test: `uint64_t -> uint64_t` with random keys. Each map runs in its own
  child process. Memory is the child peak RSS minus the baseline (the input
  arrays only).
- The whole run is pinned to one core with `taskset -c 2`. CPU 2 gets the
  fewest interrupts. Its hyperthread sibling (CPU 3) was not kept idle, but
  the machine was otherwise idle.
- 1M and 10M: median of 3 runs. 100M: 1 run. At 10M the sflat rows vary by
  3 percent or less. At 1M the find hit rows are bimodal: random order 23.2
  to 30.4 ns over 8 runs, insert order 23.7 to 28.3 (also before the
  changes: 23.0 to 28.8). The other 1M rows vary by less than 8 percent,
  except no-reserve insert (72.3 to 85.1 ns).
- An earlier run without pinning gave the same picture. Most rows were 2 to
  8 percent slower and varied more between runs.

### Results

Lower ns/op is better. Lower bytes/entry is better. **Bold** is the best
value in the row. The rows mean the same as in the Xeon results.

- The L3 cache is only 3 MiB. Even the 1M tables (about 20 to 50 MB) do not
  fit in cache, so all sizes are bound by DRAM latency and TLB misses.

**n = 1,000,000**

| Operation | sflat | emhash8 | boost_flat | boost_node | absl_flat | absl_node | std |
|---|---|---|---|---|---|---|---|
| insert (reserved) | **30.7** | 52.6 | 60.9 | 146.8 | 74.8 | 113.5 | 211.3 |
| find hit (insert order) | 23.9 | **20.5** | 27.5 | 33.0 | 25.9 | 27.8 | 77.2 |
| find hit (random order) | 29.3 | 29.6 | 27.7 | 44.3 | **26.2** | 41.5 | 95.8 |
| find miss | 11.0 | 17.8 | **9.3** | 37.9 | 18.7 | 24.7 | 126.2 |
| erase | **31.0** | 117.3 | 70.1 | 137.8 | 69.1 | 125.6 | 253.8 |
| iterate | 10.3 | **1.1** | 12.3 | 26.6 | 10.4 | 23.2 | 37.5 |
| insert (no reserve) | **72.8** | 143.1 | 73.5 | 324.6 | 82.1 | 245.3 | 350.0 |
| bytes/entry (steady) | **21.8** | 34.1 | 34.9 | 46.7 | 37.0 | 52.2 | 41.9 |
| bytes/entry (growth peak) | 40.7 | **36.3** | 51.3 | 46.7 | 54.4 | 58.5 | 45.2 |

At 1M the no-reserve table lands just after a 2x step (2.1M slots, load
0.48). This is the reason for the 40.7 bytes/entry growth peak.

**n = 10,000,000**

| Operation | sflat | emhash8 | boost_flat | boost_node | absl_flat | absl_node | std |
|---|---|---|---|---|---|---|---|
| insert (reserved) | **54.1** | 67.1 | 120.5 | 179.2 | 132.8 | 145.5 | 241.9 |
| find hit (insert order) | 35.8 | **25.0** | 34.2 | 41.7 | 45.0 | 37.1 | 91.2 |
| find hit (random order) | **34.2** | 40.0 | 34.3 | 55.7 | 45.7 | 56.2 | 116.4 |
| find miss | 24.2 | 24.4 | **19.0** | 50.8 | 37.8 | 41.5 | 150.7 |
| erase | **57.8** | 146.1 | 162.5 | 174.8 | 160.3 | 152.6 | 306.3 |
| iterate | 10.5 | **1.0** | 11.5 | 30.6 | 9.9 | 25.2 | 45.7 |
| insert (no reserve) | **76.0** | 159.5 | 124.3 | 335.0 | 134.2 | 295.2 | 400.9 |
| bytes/entry (steady) | **20.3** | 29.5 | 27.0 | 42.8 | 28.7 | 47.2 | 40.4 |
| bytes/entry (growth peak) | 30.3 | **29.6** | 40.4 | 42.8 | 42.9 | 47.2 | 41.8 |

**n = 100,000,000**

| Operation | sflat | emhash8 | boost_flat | boost_node | absl_flat | absl_node | std |
|---|---|---|---|---|---|---|---|
| insert (reserved) | **82.4** | 115.0 | 180.2 | 255.2 | 178.5 | 171.1 | 304.4 |
| find hit (insert order) | 38.2 | **37.2** | 74.5 | 80.5 | 76.2 | 74.7 | 132.4 |
| find hit (random order) | **36.4** | 97.4 | 74.5 | 139.9 | 76.6 | 145.6 | 188.0 |
| find miss | 27.2 | 37.8 | **24.9** | 115.0 | 74.9 | 85.1 | 215.0 |
| erase | **137.0** | 180.5 | 229.1 | 255.8 | 177.0 | 199.9 | 413.2 |
| iterate | 10.4 | **0.9** | 10.6 | 55.7 | 9.5 | 49.4 | 67.5 |
| insert (no reserve) | **92.8** | 197.9 | 175.7 | 409.6 | 179.0 | 340.0 | 514.8 |
| bytes/entry (steady) | **19.7** | 26.7 | 21.5 | 40.6 | 22.8 | 44.1 | 40.1 |
| bytes/entry (growth peak) | **24.0** | 26.7 | 32.2 | 40.6 | 34.2 | 44.1 | 40.1 |

100M: 1 run. The four input arrays (3.2 GB) and the largest maps fit in the
15 GiB of this machine.

### THP on and off (sflat only)

The same pinned benchmark with `-DSFLAT_USE_THP=0` (sflat then uses 4 KiB
pages like the other maps). Median of 3 runs at 1M and 10M, 1 run at 100M.
The best of the other flat maps (boost_flat or absl_flat, from the THP-off
run) is shown for reference.

| ns/op | 1M THP off | 1M THP on | 10M THP off | 10M THP on | 100M THP off | 100M THP on | best other flat, 100M |
|-------|-----------|-----------|-------------|------------|--------------|-------------|-----------------------|
| insert (reserved) | 50.9 | **30.7** | 115.6 | **54.1** | 181.4 | **82.4** | 181.2 (boost_flat) |
| find hit (random order) | 30.0 | **29.3** | 44.1 | **34.2** | 74.9 | **36.4** | 74.2 (boost_flat) |
| find miss | 12.7 | **11.0** | 32.2 | **24.2** | 40.7 | **27.2** | 25.0 (boost_flat) |
| insert (no reserve) | 96.0 | **72.8** | 137.0 | **76.0** | 187.0 | **92.8** | 174.6 (boost_flat) |
| erase | 62.8 | **31.0** | 161.0 | **57.8** | 222.0 | **137.0** | 183.9 (absl_flat) |

THP is better in every row and at every size, as on the Celeron. At 100M a
random find and a reserved insert are both about 2 times faster with THP.

With THP off, sflat is equal to boost_flat on random find hit at 100M (74.9
and 74.2 ns) but 10 percent slower than absl_flat at 1M and 29 percent
slower than boost_flat at 10M. emhash8 is then the fastest on reserved
insert at 10M and 100M (65.6 and 112.2 ns). So, as on the Celeron, most of
the lead of sflat on this machine comes from huge pages, not from the
probing scheme. Part of the rest is the load factor: boost_flat runs at
about 0.64 after `reserve`, sflat at 0.875 (see the profiling notes).

### Honest assessment

- **memory (steady state)**: sflat uses the least memory at all sizes: 8 to
  38 percent less than boost_flat, 14 to 41 percent less than absl_flat, and
  26 to 36 percent less than emhash8.
- **memory (growth peak)**: at 100M sflat has the lowest peak (24.0
  bytes/entry). At 10M it is about equal to emhash8 (30.3 and 29.6). At 1M
  the 2x step makes it 40.7, more than emhash8 (36.3).
- **insert**: sflat is the fastest at all sizes, with and without reserve
  (at 1M without reserve it is equal to boost_flat: 72.8 and 73.5 ns). At
  100M it is 1.4 times faster than emhash8 with reserve and 1.9 times
  faster than boost_flat without reserve. This is mainly the THP effect.
- **find hit (random order)**: at 100M sflat is about 2 times faster than
  boost_flat (36.4 and 74.5 ns). At 10M it is equal to boost_flat (34.2 and
  34.3). At 1M absl_flat (26.2) and boost_flat (27.7) are a little faster
  than the sflat median (29.3); the sflat runs range from 23.2 to 30.4.
- **find miss**: boost_flat is still the fastest at all sizes, but the gap
  is now 1.1 to 1.3 times (was 1.5 to 2.1 before the 32-bit overflow
  words). sflat is second at all sizes.
- **erase**: sflat is the fastest at all sizes with THP (1.3 to 2.5 times
  faster than the next map).
- **iterate**: about equal to absl_flat (up to 1.1 times slower) and faster
  than boost_flat at 1M and 10M, after the group-wise scan (was 1.4 to 1.5
  times slower than absl_flat).

### Profiling on the i5-3230M

Hardware counters per phase (`perf_event_open` around each phase, pinned to
CPU 2), for sflat, boost_flat, absl_flat and emhash8. Only two general
counters are free on this machine (NMI watchdog, hyperthreading), so the
events were collected in six runs. "MLP" is the average number of L1D
misses in flight (`l1d_pend_miss.pending / l1d_pend_miss.pending_cycles`).
Values are per operation, with the sflat code from before the changes
below.

| n = 10M | map | ns | instructions | branch mispredicts | LLC load misses | page walks | page walk cycles | MLP |
|---|---|---|---|---|---|---|---|---|
| find hit (random order) | sflat | 34.2 | 39.0 | 0.17 | 1.65 | 0.78 | 10 | 4.1 |
| | boost_flat | 36.8 | 35.6 | 0.03 | 1.58 | 2.03 | 68 | 4.1 |
| | absl_flat | 37.6 | 36.4 | 0.01 | 1.09 | 1.97 | 97 | 3.7 |
| find miss | sflat | 31.6 | 49.9 | 0.76 | 0.35 | 0.06 | 1 | 3.1 |
| | boost_flat | 19.2 | 29.0 | 0.05 | 0.75 | 0.94 | 23 | 3.7 |
| insert (reserved) | sflat | 53.2 | 90.1 | 0.16 | 0.86 | 0.12 | 1 | 1.5 |
| | boost_flat | 121.3 | 82.7 | 0.04 | 0.94 | 4.02 | 47 | 1.2 |
| iterate | sflat | 14.0 | 43.9 | 1.20 | 0.00 | 0.00 | 0 | 1.5 |
| | absl_flat | 9.6 | 25.2 | 1.10 | 0.06 | 0.01 | 0 | 2.2 |

What the counters show:

- **The lead of sflat here is page walks.** All maps miss the cache about
  equally often, but boost_flat and absl_flat (4 KiB pages) spend 68 to 97
  cycles per find in page walks, sflat (THP) about 10. At 100M sflat still
  needs about 1.8 walks per find: Ivy Bridge has only 32 TLB entries for
  2 MiB pages.
- **Hit speed is bound by the out-of-order window.** Instructions per find
  times MLP is about 160 to 170 for every variant, close to the 168-entry
  reorder buffer. Every extra instruction in the hit path lowers MLP.
- **Misses were slow because of mispredicts on a DRAM-bound branch.** 9.5
  percent of misses probed a second group (8-bit overflow filter), and that
  branch waits for the control bytes from DRAM. A version that never probes
  further (correct for misses only) ran 15.5 ns instead of 29.1 ns at 10M.
- **Iteration mispredicted on almost every slot** (a byte loop over mixed
  full and free slots after erases).
- **Erase cost is the write.** At 100M a find alone takes 48 ns; adding any
  one-byte write (to the control byte or to the slot) makes it about
  135 ns, with no change in kernel time or page faults. All maps pay this.

Changes made from these results (10M, sflat, ns/op):

| Change | Row | Before | After |
|---|---|---|---|
| Read the overflow word together with the control bytes; test "empty slot or clear bit" without a short-circuit branch | find miss | 34.2 | 29.3 |
| Scan a whole group per step in iteration (SIMD), not a byte | iterate | 14.7 | 10.3 |
| Overflow filter 8 bits to 32 bits per group: second-group probes 9.5 to 2.7 percent of misses | find miss | 29.3 | 26.0 |

Tried and not kept:

- **Slot prefetch on x86** (`SFLAT_PREFETCH_SLOTS=1`): find hit 15 to 25
  percent slower, misses much slower.
- **`erase(key)` without computing the next iterator**: no measurable gain.
- **Overflow words inside the control cache line** (blocks of 64 bytes: 3
  groups of 16 control bytes and 3 overflow words). Misses were as fast as
  with the separate 32-bit array (29.4 vs 29.1 ns at 100M), but the block
  address arithmetic added about 10 instructions per find, so MLP fell from
  4.1 to 3.4 and hits got 17 percent slower; iteration and growth also got
  slower.
- **Lower max load factor** (THP off, 10M): 0.64 instead of 0.875 makes a
  random find hit 41.2 to 37.0 ns. boost_flat runs at about 0.64 after
  `reserve` (power-of-two group count); this is most of its lead over
  sflat with 4 KiB pages. It costs about 30 percent more memory, so the
  default stays 0.875.
## Apple M2, MacBook Air (macOS)

### Test environment

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

### Results

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
table for every map. sflat has no mmap path on macOS (see
[Growth memory](README.md#growth-memory-linux)),
so its page release does not help here. The steady-state row is not
affected.

### Why find is slower on the M2, and the find tuning

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

### NEON compared with SWAR

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

### Assessment (M2)

- **memory (steady state)**: sflat uses the least memory at all sizes
  (19.5 to 19.6 bytes/entry): 9 to 42 percent less than boost_flat, 14 to
  45 percent less than absl_flat, and 27 to 40 percent less than emhash8.
- **memory (growth peak)**: equal to boost_flat at all sizes. All maps
  pay for the macOS malloc cache (see above).
- **find hit (random order)**: sflat is the fastest at 1M (5.3 ns). At 10M
  and 100M it is 1.2 times slower than boost_flat and 1.4 to 1.5 times
  slower than absl_flat (before the tuning: 1.5 to 2.0 times). The x86-64
  machines differ mainly because of THP: sflat uses 2 MiB pages on Linux,
  and there was its lead at large sizes (Xeon, 100M: 45.8 ns with THP,
  55.9 without, boost_flat 46.7). macOS has no such path.
- **insert**: not tuned yet. boost_flat is the fastest. sflat is second
  or third for reserved inserts and 1.2 to 1.4 times slower than
  boost_flat without reserve.
- **find miss**: boost_flat is 2.3 to 5.2 times faster, for the same
  reason as on the x86-64 machines (load factor 0.875, so the overflow bits
  are often set).
- **erase**: sflat is the fastest at 1M; at 10M and 100M boost_flat is
  1.4 to 1.5 times faster.

## Large-scale estimates (not measured)

The table below extrapolates from the measured steady-state bytes/entry.

| Entries | sflat (about 20 B/entry) | boost_flat (about 30 B/entry) |
|---------|--------------------------|--------------------------|
| 100M    | about 2.0 GB             | about 3.0 GB             |
| 1B      | about 20 GB              | about 30 GB              |
| 100B    | about 2.0 TB             | about 3.0 TB             |

With page release, the growth peak is about the size of the new table
(2x, 1.5x or 1.25x of the old table) plus the control bytes. The measured
peak is 23.8 bytes/entry at 100M (Xeon), 24.0 at 100M (i5-3230M, with
the 32-bit overflow words) and 23.7 at 50M (Celeron). The actual values depend on the allocator and on the key and
value sizes.
