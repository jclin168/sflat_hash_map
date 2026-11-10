# Comprehensive sflat reevaluation

## Scope and outcome

Test sflat only. Use main commit `6118325b6824cc33f8d15bae06bc714235e3451f` with std::hash. Rebuild the full map_benchmark executable from source. Keep the original input sizes and loop counts.

All 20 original cases passed: 17 timed cases and three diagnostics. All 18 controlled processes also passed. They cover 1M, 10M, and 100M entries, with three repeats per size and two cases per repeat. Result checks passed in each measured phase. No process reached the timeout or memory watchdog. The measurements required no library source change.

Copy took 0.799127 s, compared with 6.459790 s in the historical run. The original InsertHugeInt case took 15.333994 s and used 1.215 GiB process peak RSS. InsertHugeInt makes 100M insertion attempts and then clears the table. It is distinct from the controlled case with 100M unique live entries.

The equal-weight geometric mean time ratio across the 15 core cases is 0.8810 times the historical sflat time. Excluding Copy, the ratio is 1.0137. These compare two runs of sflat. They are not a new ranking against other containers. The historical run and this run used the same machine configuration, but they ran at different times.

## Original cases

Each timed case has one fresh process. Time is the sum of its measured phases. It excludes work outside the benchmark timers. Negative change means less time. RSS and virtual peak values cover the whole process.

| Case | Before, s | Latest, s | Time change | Peak RSS, MiB | Observed virtual peak, MiB |
|---|---:|---:|---:|---:|---:|
| ShowHash | diagnostic | PASS | — | 0.1 | 0.6 |
| ShowHashString | diagnostic | PASS | — | 0.1 | 0.6 |
| ShowIsStableReference | diagnostic | PASS | — | 0.3 | 3.2 |
| CtorDtorEmptyMap | 0.000000 | 0.000000 | -33.9% | 8.3 | 0.6 |
| CtorDtorSingleEntryMap | 0.461418 | 0.436351 | -5.4% | 8.3 | 6.3 |
| Copy | 6.459790 | 0.799127 | -87.6% | 75.9 | 81.2 |
| GameOfLife_growing | 3.087540 | 3.144230 | +1.8% | 8.3 | 8.2 |
| GameOfLife_stabilizing | 8.501796 | 8.525689 | +0.3% | 8.3 | 6.6 |
| AccidentallyQuadratic | 0.859112 | 0.920786 | +7.2% | 305.9 | 374.3 |
| IterateIntegers | 5.749150 | 5.872720 | +2.1% | 8.6 | 7.9 |
| RandomDistinct2 | 6.495098 | 7.691601 | +18.4% | 623.9 | 884.3 |
| InsertHugeInt | 15.771339 | 15.333994 | -2.8% | 1243.9 | 1760.3 |
| RandomInsertErase | 13.113130 | 12.094130 | -7.8% | 287.9 | 420.3 |
| RandomInsertEraseStrings | 24.677510 | 23.913960 | -3.1% | 74.6 | 105.3 |
| knucleotide | 4.455225 | 3.483250 | -21.8% | 129.1 | 135.5 |
| RandomFindString | 30.811420 | 26.092310 | -15.3% | 18.8 | 24.7 |
| RandomFindString_1000000 | 37.827270 | 67.751360 | +79.1% | 88.5 | 132.8 |
| RandomFind_200 | 46.245800 | 33.875380 | -26.7% | 8.9 | 6.3 |
| RandomFind_2000 | 37.926420 | 40.376080 | +6.5% | 8.9 | 6.4 |
| RandomFind_500000 | 52.488950 | 59.666310 | +13.7% | 22.0 | 34.3 |

The baseline is the retained sflat result from the original comprehensive run. It uses header commit `f163c3d`. Hash diagnostic output is byte-for-byte equal in the old and new runs. Stable-reference output is a diagnostic observation, not an API guarantee. Empty-constructor timing can be removed by compiler optimization.

## Controlled insert and find

Use unique 64-bit keys, 64-bit values, std::hash, and input seed 42. The reserved case times reserve plus insert, random hit lookup, then miss lookup. The growth case times insert without reserve. There is no clear operation in these cases. Times below are medians of three fresh processes. The CSV also includes each range and the memory metrics.

| Live entries | Reserve + insert, s | Insert without reserve, s | Random find hit, s | Find miss, s |
|---|---:|---:|---:|---:|
| 1,000,000 | 0.017739 | 0.047109 | 0.010325 | 0.006989 |
| 10,000,000 | 0.232278 | 0.482034 | 0.245393 | 0.152659 |
| 100,000,000 | 3.837670 | 5.702360 | 3.396280 | 2.335080 |

| Live entries | Reserved process peak RSS, MiB | Reserved virtual peak, MiB | Growth process peak RSS, MiB | Growth virtual peak, MiB |
|---|---:|---:|---:|---:|
| 1,000,000 | 28.8 | 33.9 | 47.5 | 68.0 |
| 10,000,000 | 267.6 | 272.5 | 364.2 | 496.6 |
| 100,000,000 | 2632.9 | 2639.2 | 3030.9 | 4059.3 |

Memory is not table-only storage. It includes the input key vector, executable, allocator, and temporary storage. The key vector alone uses about 7.6, 76.3, and 762.9 MiB at the three sizes. RSS measures resident pages. Virtual peak measures address space. The two quantities have different meanings. Virtual peaks are sampled at 5 ms intervals; short peaks can be missed. Small-process RSS can be dominated by startup memory.

## Checks for large time increases

Select at most the three largest time increases above 15% against historical data. Exclude cases whose historical time is below 0.1 s. Run one fresh baseline process and one fresh latest process for each selected case. Keep the first full-run results in the table above. This check does not replace them.

| Case | Fresh baseline, s | Fresh latest, s | Time change |
|---|---:|---:|---:|
| RandomFindString_1000000 | 35.521090 | 34.968230 | -1.6% |
| RandomDistinct2 | 6.997207 | 7.554554 | +8.0% |

The fresh check for RandomFindString_1000000 does not repeat the initial large time increase. Its time change is -1.6%.

The fresh check for RandomDistinct2 changes time by +8.0%. Compare both samples, rather than only the first result.

A single pair can check the direction of a large difference. It cannot establish a stable small speed difference. Do not infer a code defect from a comparison with an old run alone. The full-run phase CSV shows where each time difference occurs.

## Configuration and limits

Intel Xeon Platinum 8573C; Linux x86-64; GCC 14.2; C++17. Flags: `-O3 -DNDEBUG -march=native -DSFLAT_PROCESS_SEED=1`. Use default sflat growth, copy slack, load factor, and huge-page macros. Processes run sequentially on CPU 0. No compile or correctness workload runs at the same time as timed measurements.

The cgroup permits two CPU cores and 8 GiB memory. Each process has a 180 s timeout and a 7 GiB RSS watchdog. A second watchdog stops at 7.5 GiB cgroup memory. There is no virtual-address limit. The runner records memory events before and after each process. No OOM event occurred.

The original suite covers integer, string, copy, iteration, insert/erase, and application workloads. It does not test every key/value type, custom allocator, custom hasher, platform, thread pattern, or macro combination. StringHashBench and disabled Strings.cpp are not registered cases. FlatMap64 is an additional profile, outside the 20 original cases. The exact latest headers already passed the copy change's correctness and sanitizer checks; this task measures the benchmark result checks.

## Files and reproduction

Download [the complete run archive](complete-run.zip) for raw results, logs, checkpoints, and runner scripts. See [SHA256SUMS](SHA256SUMS) for file hashes. The archive is unchanged from the local run. Its report describes the state before publication. Paths below refer to files inside the archive unless a link is provided.

- [case-comparison.csv](case-comparison.csv): all original case times and process memory.
- [phase-comparison.csv](phase-comparison.csv): old/new measured phases and expected result checks.
- [controlled-summary.csv](controlled-summary.csv): three-repeat insert/find medians and ranges.
- [followup-comparison.csv](followup-comparison.csv): fresh baseline checks for large time increases.
- `results/`: full-run environment, stdout, stderr, JSON checkpoints, and raw phase CSV.
- `controlled/`: raw results for each size and repeat.
- `followup/`: raw fresh-baseline and latest checks.
- `historical/`: the 20 retained historical sflat checkpoints used in the comparison.
- `build-command.sh`: the exact compile command.
- [validation.json](validation.json): source, binary, result, and memory-event checks.

The build uses the same frozen benchmark source as the [comprehensive comparison](https://github.com/jclin168/map_benchmark/blob/master/docs/results/2026-11-10-stdhash/REPORT.md). The first include directory selects latest sflat. The old embedded sflat dependency is not the measured header; `sflat_actual_include_override` identifies the actual header revision. Environment files include full source and executable SHA-256 values.

In the original workspace, run `build-command.sh`, then `run_comprehensive.py --build /workspace/sflat-rerun/build --out /path/to/new-results`. Run controlled cases with `MAP_BENCH_N` set to the size and `MAP_BENCH_SEED=42`. The runners use absolute paths from the measured workspace. On another machine, update those paths and use the recorded dependency revisions. Results can change with CPU, compiler, operating system, page policy, and machine load.
