# 250M insert-only memory study: virtual address space and RSS

On November 9, 2026, **sflat and Google sparse completed 250 million inserts** in this 8 GiB Linux environment. sflat finished at **4.818 GiB RSS**, with a **5.031 GiB RSS peak**; Google sparse finished at **4.294 GiB RSS**. Boost flat, Ankerl dense, Abseil flat and Google dense were stopped by the environment memory watchdog before reaching the target. Their stop snapshots are listed separately, not as completed 250M results.

This is a single-run memory study of `uint64_t -> uint64_t` with no reserve. It is separate from the timing tables in [BENCHMARK.md](../../../BENCHMARK.md). **All numbers below are whole-process values, without subtracting the startup baseline**, in GiB (2^30 bytes).

## Completed 250M results

| Map | Entries | Final VmSize GiB | Final RSS GiB | VmPeak GiB | RSS peak GiB |
|---|---:|---:|---:|---:|---:|
| sflat | 250,000,000 | 4.822 | 4.818 | 8.035 | 5.031 |
| Google sparse | 250,000,000 | 4.298 | 4.294 | 4.298 | 4.294 |

Final values were read after the last insertion, with the map still alive. The final size was checked against 250,000,000. Both containers received the same key sequence. Google sparse used about 10.9% less final RSS than sflat in this run. This study does not establish a timing ranking.

## Stopped attempts: these are not 250M measurements

| Map | Last confirmed inserted count | VmSize at stop GiB | RSS at stop GiB | Observed VmPeak GiB | Observed RSS peak GiB |
|---|---:|---:|---:|---:|---:|
| Boost flat | ≥220,000,000 | 12.006 | 6.964 | ≥12.006 | ≥6.964 |
| Ankerl dense | ≥210,000,000 | 8.131 | 6.965 | ≥8.131 | ≥6.965 |
| Abseil flat | ≥230,000,000 | 12.756 | 6.967 | ≥12.756 | ≥6.967 |
| Google dense | ≥130,000,000 | 12.006 | 6.964 | ≥12.006 | ≥6.964 |

All four stop reasons were `cgroup_watchdog`: the runner observed total environment memory above 7.5 GiB and sent SIGKILL. These were deliberate watchdog stops, not reported native allocation failures. No system cgroup OOM or OOM-kill events occurred.

The count is the last flushed 10M checkpoint, so the true count at termination may be higher. VmSize and RSS at stop come from the same `/proc/PID/status` sample immediately before SIGKILL. Observed peaks for stopped processes are lower bounds; allocations can continue between sampling and termination. **These rows cannot be used to rank completed 250M memory use, infer the full peak required to finish, or establish an exact capacity limit for an 8 GiB machine.**

## Why virtual address space and RSS differ

- **VmSize** counts current virtual mappings, including reserved pages that are not resident. It is not a physical-memory measurement.
- **RSS** counts current resident pages.
- **VmPeak** is the historical maximum VmSize. RSS peak is the maximum from process `getrusage` and monitored `VmHWM`. The two peaks need not occur at the same time.

sflat reached 8.035 GiB of virtual address space during growth while its observed RSS peak was 5.031 GiB. Its default Linux allocator reserves space for old/new arrays and reclaims migrated pages. A virtual-address-space cap can therefore reject an allocation while resident use is still below the physical-memory budget. This run left RLIMIT_AS unlimited. It does not imply that sflat can fit every later growth step into this budget.

Completed runs read VmSize/VmPeak from `/proc/self/status` and RSS from `smaps_rollup`. Watchdog stop snapshots read both VmSize and RSS from `/proc/PID/status`. Kernel RSS accounting methods can differ slightly. Startup RSS and VmSize baselines remain in the original per-map CSVs; earlier reports that subtract a baseline use a different memory column.

## Workload and environment

- Intel Xeon Platinum 8573C; Linux x86-64 shared VM; three visible vCPUs. Each child was pinned to CPU 0 and runs were sequential.
- GCC 14.2; C++17; `-O3 -DNDEBUG -march=native`; common `std::hash` adapter; default container load factors and allocators. sflat used `SFLAT_PROCESS_SEED=1`.
- Target: 250,000,000 distinct 64-bit keys and 64-bit values, inserted from an empty map with no reserve and no find phase. A bijective permutation generates keys on demand; no O(N) input vector is allocated. Google sentinel values are excluded for all six maps.
- One independent process per container, in the archived runner order; no repeated-run confidence interval. Elapsed times in raw logs include key generation, progress reporting and monitoring overhead; do not mix them with pure insertion timings.
- Environment cgroup hard limit: 8 GiB; no swap. Process RSS watchdog: 7 GiB; total environment `memory.current` watchdog: 7.5 GiB. The latter includes other processes and page cache and can fire before the process RSS threshold.
- Approximately 5 ms polling; a sampled watchdog, not an enforced Linux RLIMIT_RSS cap. Actual peaks may overshoot the thresholds.
- RLIMIT_AS soft/hard: unlimited. THP system policy: `madvise`; the normal sflat mmap, page release and THP code paths were retained.

## Versions

| Component | Measured revision |
|---|---|
| map_benchmark | `7acf8473809b93e5b504a41354238e9a61feb4ea` |
| sflat | `f163c3d353e7c0b54dc73be0fb8b481c59aacc08` |
| google_sparsehash | `f93c0c69e959c1c77611d0ba8d107aa971338811` |
| ankerl | `7c58a8b7a48e790fe72f6508eb3b46317cbba9d5` |
| abseil | `d38452e1ee03523a208362186fd42248ff2609f6` |
| boost | `f976a2b7b525300c0ff63ace104d2d83793d3153` |

Ankerl was 5.3.3, Abseil LTS 20250814.1, Google sparsehash 2.0.4 and Boost 1.93 development headers. The measured sflat source was `f163c3d`; this publication does not retest later revisions. The exact patched map_benchmark harness is archived below.

## Data and reproduction

- [Summary CSV](raw/summary.csv): completed measurements and separately labeled stop snapshots.
- [Raw results and stop snapshots](raw/results.json): child exit codes, stop reasons, sampled maxima and streaming-executable SHA-256 values.
- [Environment manifest](raw/environment.json): dependency commits, compiler, flags, CPU and memory policies. Its top-level `binary_sha256` entries identify the pre-existing harness targets; `raw/results.json` contains the actual streaming-insert executable hashes.
- [Measured C++ source](raw/insert_stream.measured.cpp), individual map checkpoint CSVs and stderr files in [raw/](raw/).
- [Before](raw/memory.events.before) / [after](raw/memory.events.after) cgroup counters and [validation notes](raw/validation.txt).
- [Original Traditional Chinese report](raw/REPORT.zh-TW.md).
- [Reproduction instructions](REPRODUCE.md) and [complete map_benchmark integration patch](map_benchmark.patch).

The patch includes the exact streaming probe, runner, adapters, selected build configuration and reporting scripts. The upstream map_benchmark portions retain their [MIT license](map_benchmark.LICENSE); dependency projects retain their own licenses. Absolute workspace paths in the original manifest describe the measured environment and are not portable build locations.
