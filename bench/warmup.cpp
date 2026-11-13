// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jian-Cheng Lin
// Warm-up before a benchmark run: touch GiB of memory twice (the host backs
// guest memory on first use, which is slow in a VM) and spin the CPU for a
// few seconds so the clock is stable. Usage: warmup [GiB] [spin seconds]
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

int main(int argc, char** argv) {
  const size_t gib = argc > 1 ? strtoull(argv[1], nullptr, 10) : 12;
  const double spin = argc > 2 ? atof(argv[2]) : 5.0;
  const size_t bytes = gib << 30;
  for (int pass = 0; pass < 2; ++pass) {
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<char> buf(bytes);
    for (size_t i = 0; i < bytes; i += 4096) buf[i] = 1;
    const double s =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0)
            .count();
    std::printf("touch pass %d: %zu GiB in %.2f s (%.2f us per 4 KiB page)\n",
                pass + 1, gib, s, s * 1e6 / (bytes / 4096.0));
  }
  const auto t0 = std::chrono::steady_clock::now();
  volatile uint64_t x = 1;
  while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0)
             .count() < spin)
    for (int i = 0; i < 100000; ++i) x = x * 6364136223846793005ULL + 1;
  std::printf("cpu spin: %.1f s\n", spin);
  return 0;
}
