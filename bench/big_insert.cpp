// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jian-Cheng Lin
// Large no-reserve insert: N random uint64_t keys (default 200M) into an
// empty map, for sflat, boost::unordered_flat_map, ankerl::unordered_dense
// and absl::flat_hash_map. Each map uses its default hash.
//
// Each run is a forked child. Measured per map:
//   time        insert loop (keys made before the timer), total and ns/op
//   worst step  the slowest block of STEP inserts (default 1M): growth
//               stalls. With a small STEP (1000) it shows the longest pause
//               of one growth: one rehash (boost, absl, ankerl) against the
//               migration steps of sflat.
//   RSS         peak (ru_maxrss from wait4) and at the end (VmRSS)
//   virtual     peak (VmPeak) and at the end (VmSize)
//   huge pages  AnonHugePages at the end (smaps_rollup)
// RSS, virtual sizes and huge pages are given minus a baseline child that
// has only the key array.
//
// Warm-up: before the runs, a child touches and frees most of the memory
// (in a VM the first touch of a page is slow, and the first map in the run
// order would pay it); before each run, a shorter warm-up child.
//
// THP: sflat asks for huge pages itself (SFLAT_USE_THP). For the maps that
// use malloc, run with GLIBC_TUNABLES=glibc.malloc.hugetlb=1 (glibc 2.35
// and later) when /sys/kernel/mm/transparent_hugepage/enabled is madvise.
//
//   ./big_insert [n] [runs]       default 200000000 3
//   env: ONLY=sflat,boost         the maps to run
//        STEP=1000                inserts per timed block
#include <sflat/flat_hash_map.hpp>

#include <boost/unordered/unordered_flat_map.hpp>
#ifdef HAS_UDM
#include <ankerl/unordered_dense.h>
#endif
#ifdef HAS_ABSL
#include <absl/container/flat_hash_map.h>
#endif

#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace {

uint64_t splitmix64(uint64_t& s) {
  uint64_t z = (s += 0x9E3779B97F4A7C15ULL);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
  return z ^ (z >> 31);
}

// A field of /proc/self/status or smaps_rollup, in KiB (-1 if absent).
long proc_kib(const char* file, const char* field) {
  std::ifstream f(file);
  std::string line;
  const size_t n = std::strlen(field);
  while (std::getline(f, line))
    if (line.compare(0, n, field) == 0) return std::atol(line.c_str() + n);
  return -1;
}

struct Result {  // written by the child into a pipe
  double seconds = 0, worst_step_ms = 0;
  long rss_end = 0, vm_peak = 0, vm_end = 0, huge = 0;
  size_t size = 0;
  long rss_peak = 0;  // filled in by the parent
};

std::vector<uint64_t> make_keys(size_t n) {
  std::vector<uint64_t> k(n);
  uint64_t s = 0x123456789ABCDEFULL;
  for (auto& x : k) x = splitmix64(s);
  return k;
}

template <typename Map>
Result insert_child(size_t n) {
  const std::vector<uint64_t> keys = make_keys(n);
  using Clock = std::chrono::steady_clock;
  const char* st = std::getenv("STEP");
  const size_t kStep = st && *st ? std::strtoull(st, nullptr, 10) : 1000000;
  Result r;
  Map m;
  const auto t0 = Clock::now();
  auto ts = t0;
  for (size_t i = 0; i < n; i += kStep) {
    const size_t e = std::min(n, i + kStep);
    for (size_t j = i; j < e; ++j) m.emplace(keys[j], j);
    const auto t = Clock::now();
    r.worst_step_ms = std::max(
        r.worst_step_ms,
        std::chrono::duration<double, std::milli>(t - ts).count());
    ts = t;
  }
  r.seconds = std::chrono::duration<double>(ts - t0).count();
  r.size = m.size();
  r.rss_end = proc_kib("/proc/self/status", "VmRSS:");
  r.vm_peak = proc_kib("/proc/self/status", "VmPeak:");
  r.vm_end = proc_kib("/proc/self/status", "VmSize:");
  r.huge = proc_kib("/proc/self/smaps_rollup", "AnonHugePages:");
  return r;  // the map stays alive until here
}

Result baseline_child(size_t n) {
  const std::vector<uint64_t> keys = make_keys(n);
  Result r;
  r.size = keys.size() + (keys[n / 2] & 0);
  r.rss_end = proc_kib("/proc/self/status", "VmRSS:");
  r.vm_peak = proc_kib("/proc/self/status", "VmPeak:");
  r.vm_end = proc_kib("/proc/self/status", "VmSize:");
  r.huge = proc_kib("/proc/self/smaps_rollup", "AnonHugePages:");
  return r;
}

// Touch and free `bytes` of memory.
Result warmup_child(size_t bytes) {
  std::vector<char> buf(bytes);
  for (size_t i = 0; i < bytes; i += 4096) buf[i] = 1;
  Result r;
  r.size = buf[bytes / 2];
  return r;
}

Result run_child(Result (*fn)(size_t), size_t n) {
  std::fflush(stdout);
  int fd[2];
  if (pipe(fd) != 0) std::abort();
  const pid_t pid = fork();
  if (pid == 0) {
    close(fd[0]);
    const Result r = fn(n);
    if (write(fd[1], &r, sizeof(r)) != static_cast<ssize_t>(sizeof(r)))
      _exit(1);
    _exit(0);
  }
  close(fd[1]);
  Result r;
  const bool ok = read(fd[0], &r, sizeof(r)) == static_cast<ssize_t>(sizeof(r));
  close(fd[0]);
  int status = 0;
  struct rusage ru;
  std::memset(&ru, 0, sizeof(ru));
  while (wait4(pid, &status, 0, &ru) < 0) {
  }
  if (!ok || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    std::fprintf(stderr, "child failed\n");
    std::exit(1);
  }
  r.rss_peak = ru.ru_maxrss;  // KiB on Linux
  return r;
}

struct Entry {
  const char* name;
  Result (*fn)(size_t);
};

const Entry kMaps[] = {
    {"sflat", insert_child<sflat::flat_hash_map<uint64_t, uint64_t>>},
    {"boost", insert_child<boost::unordered_flat_map<uint64_t, uint64_t>>},
#ifdef HAS_UDM
    {"ankerl", insert_child<ankerl::unordered_dense::map<uint64_t, uint64_t>>},
#endif
#ifdef HAS_ABSL
    {"absl", insert_child<absl::flat_hash_map<uint64_t, uint64_t>>},
#endif
};

double median(std::vector<double> v) {
  std::sort(v.begin(), v.end());
  return v[v.size() / 2];
}

}  // namespace

int main(int argc, char** argv) {
  const size_t n = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 200000000;
  const int runs = argc > 2 ? std::atoi(argv[2]) : 3;
  std::vector<Entry> maps;
  const char* only = std::getenv("ONLY");
  for (const Entry& e : kMaps) {
    const std::string list = std::string(",") + (only ? only : "") + ",";
    if (only && *only &&
        list.find(std::string(",") + e.name + ",") == std::string::npos)
      continue;
    maps.push_back(e);
  }
  const long pages = sysconf(_SC_PHYS_PAGES), psize = sysconf(_SC_PAGE_SIZE);
  const size_t phys = static_cast<size_t>(pages) * static_cast<size_t>(psize);
  const char* tun = std::getenv("GLIBC_TUNABLES");
  const char* st = std::getenv("STEP");
  std::printf("# n=%zu runs=%d  STEP=%s  GLIBC_TUNABLES=%s\n", n, runs,
              st ? st : "1000000", tun ? tun : "");
  {
    std::ifstream f("/sys/kernel/mm/transparent_hugepage/enabled");
    std::string l;
    std::getline(f, l);
    std::printf("# THP enabled: %s\n", l.c_str());
  }
  // Warm-up: touch 3/4 of the memory twice.
  for (int i = 0; i < 2; ++i) run_child(warmup_child, phys / 4 * 3);
  const Result base = run_child(baseline_child, n);
  std::printf("# baseline (keys only): peak RSS %.2f GiB, VmPeak %.2f GiB, "
              "huge %.2f GiB\n", base.rss_peak / 1048576.0,
              base.vm_peak / 1048576.0, base.huge / 1048576.0);
  std::vector<std::vector<Result>> res(maps.size());
  for (int r = 0; r < runs; ++r) {
    for (size_t k = 0; k < maps.size(); ++k) {
      const size_t m = (k + r) % maps.size();  // rotate the order
      run_child(warmup_child, phys / 2);
      const Result x = run_child(maps[m].fn, n);
      res[m].push_back(x);
      std::printf(
          "run %d %-7s %6.2f s %6.1f ns/op  worst step %8.1f ms  "
          "RSS peak %6.2f end %6.2f GiB  VM peak %6.2f end %6.2f GiB  "
          "huge %6.2f GiB  size %zu\n",
          r + 1, maps[m].name, x.seconds, x.seconds * 1e9 / n,
          x.worst_step_ms, (x.rss_peak - base.rss_peak) / 1048576.0,
          (x.rss_end - base.rss_end) / 1048576.0,
          (x.vm_peak - base.vm_peak) / 1048576.0,
          (x.vm_end - base.vm_end) / 1048576.0, (x.huge - base.huge) / 1048576.0, x.size);
    }
  }
  // Median of the runs; memory minus the baseline, also in bytes/entry.
  std::printf("\n| Map | time s | ns/op | worst 1M step ms | peak RSS GiB | "
              "end RSS GiB | peak VM GiB | end VM GiB | peak RSS B/entry | "
              "end RSS B/entry | huge pages GiB |\n"
              "|---|---|---|---|---|---|---|---|---|---|---|\n");
  for (size_t m = 0; m < maps.size(); ++m) {
    std::vector<double> t, w, rp, re, vp, ve, h;
    for (const Result& x : res[m]) {
      t.push_back(x.seconds);
      w.push_back(x.worst_step_ms);
      rp.push_back((x.rss_peak - base.rss_peak) * 1024.0);
      re.push_back((x.rss_end - base.rss_end) * 1024.0);
      vp.push_back((x.vm_peak - base.vm_peak) * 1024.0);
      ve.push_back((x.vm_end - base.vm_end) * 1024.0);
      h.push_back((x.huge - base.huge) * 1024.0);
    }
    const double g = 1024.0 * 1024 * 1024;
    std::printf("| %s | %.2f | %.1f | %.1f | %.2f | %.2f | %.2f | %.2f | "
                "%.1f | %.1f | %.2f |\n",
                maps[m].name, median(t), median(t) * 1e9 / n, median(w),
                median(rp) / g, median(re) / g, median(vp) / g,
                median(ve) / g, median(rp) / n, median(re) / n,
                median(h) / g);
  }
  return 0;
}
