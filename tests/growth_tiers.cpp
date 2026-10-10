// Growth tiers with small limits, so the test needs little memory.
// 16-byte slots + 1 control byte = 17 bytes per slot:
//   below 340 KB (20000 slots) the table grows 2x,
//   below 1.7 MB (100000 slots) it grows 1.5x, else 1.25x.
#define SFLAT_GROW_2X_BELOW (20000ULL * 17)
#define SFLAT_GROW_1_5X_BELOW (100000ULL * 17)
#include <sflat/flat_hash_map.hpp>

#include <cstdio>
#include <cstdlib>

#define CHECK(cond)                                                \
  do {                                                             \
    if (!(cond)) {                                                 \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
      std::abort();                                                \
    }                                                              \
  } while (0)

int main() {
  sflat::flat_hash_map<uint64_t, uint64_t> m;
  size_t prev = 0;
  int steps[3] = {0, 0, 0};
  for (uint64_t i = 0; i < 1000000; ++i) {
    m.emplace(i * 0x9E3779B97F4A7C15ULL, i);
    const size_t c = m.bucket_count();
    if (c != prev) {
      if (prev >= 256) {
        const double f = static_cast<double>(c) / static_cast<double>(prev);
        if (prev < 20000) {
          CHECK(f > 1.95 && f < 2.01);
          ++steps[0];
        } else if (prev < 100000) {
          CHECK(f > 1.45 && f < 1.51);
          ++steps[1];
        } else {
          CHECK(f > 1.22 && f < 1.26);
          ++steps[2];
        }
      }
      prev = c;
    }
  }
  CHECK(steps[0] > 0 && steps[1] > 0 && steps[2] > 0);
  CHECK(m.size() == 1000000);
  for (uint64_t i = 0; i < 1000000; ++i) {
    auto it = m.find(i * 0x9E3779B97F4A7C15ULL);
    CHECK(it != m.end() && it->second == i);
  }
  std::printf("growth tiers OK (2x: %d steps, 1.5x: %d, 1.25x: %d)\n",
              steps[0], steps[1], steps[2]);
  return 0;
}
