# Reproduce the 250M memory study

The archived numbers are measurements already performed, not estimates. Reproduction requires Linux with `/proc`, cgroup v2 memory files, GCC/Clang with C++17, Python 3, CMake 3.16 or newer, GNU make and the usual C/C++ build tools. Matching GCC 14.2, dependency commits and CPU improves comparability; different machines can produce different peaks and timings.

Use a separate checkout of map_benchmark; the patch does not modify sflat's implementation. From the sflat checkout, set the report path before changing directories:

```sh
report_dir="$PWD/docs/benchmarks/memory-250m"
git clone https://github.com/martinus/map_benchmark.git map_benchmark-memory
cd map_benchmark-memory
git checkout 7acf8473809b93e5b504a41354238e9a61feb4ea
git apply --check "$report_dir/map_benchmark.patch"
git apply "$report_dir/map_benchmark.patch"
# Record the patched gitlinks in this disposable checkout before submodule update.
git add .gitmodules external/jclin168__sflat_hash_map external/google__sparsehash   external/martinus__unordered_dense external/abseil__abseil-cpp
git submodule update --init   external/jclin168__sflat_hash_map external/google__sparsehash   external/martinus__unordered_dense external/abseil__abseil-cpp
```

To match the measured Boost headers, clone the recorded modular Boost revision and initialize its submodules (this fetches a large source tree):

```sh
git clone https://github.com/boostorg/boost.git boost-source
git -C boost-source checkout f976a2b7b525300c0ff63ace104d2d83793d3153
git -C boost-source submodule update --init --recursive
MAP_BENCH_BOOST_SOURCE_DIR="$PWD/boost-source" ./tools/build_flat64.sh
ctest --test-dir build-flat64 --output-on-failure
```

Alternatively use an installed Boost >= 1.81 and omit the source override, but that will not reproduce the exact measured dependency revision. The patch's docs/FLAT64.md describes optional CMake/compiler/source overrides. The other four required dependency submodules are pinned by the patch.

Validate small input before the full run:

```sh
python3 tools/run_insert_stream.py --entries 1000 --limit-gib 0   --rss-limit-gib 7 --cgroup-limit-gib 7.5 --out results-flat64-vm-rss-smoke
```

Then run one fresh, sequential process per map with the archived physical-memory watchdog settings:

```sh
python3 tools/run_insert_stream.py --entries 250000000 --limit-gib 0   --rss-limit-gib 7 --cgroup-limit-gib 7.5 --out results-flat64-250m-vm-rss
python3 tools/summarize_insert_vm_rss.py results-flat64-250m-vm-rss
```

`--limit-gib 0` leaves RLIMIT_AS unchanged; check that the inherited limit is unlimited, as it was in the archived run. Process RSS is sampled every approximately 5 ms and compared with 7 GiB; total cgroup memory.current is compared with 7.5 GiB. The original environment also had an 8 GiB hard cgroup cap and no swap. If you want to match that resource envelope, provision those environment limits independently; the Python runner does not create or configure a cgroup.

Use a fresh output directory. The runner builds separate streaming executables, writes checkpoint CSVs every 10M inserts, records completed/stop states and takes a paired VmSize/RSS snapshot before a watchdog kill. The summary script verifies six results and separates completed 250M values from incomplete attempts.

On a larger machine, raising both watchdog budgets can allow the other containers to finish; those become a different experiment. Do not relabel partial stop snapshots as completed 250M values. The archived run is a memory comparison, not a repeat-based insertion-speed ranking.
