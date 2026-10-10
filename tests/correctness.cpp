// Correctness tests for sflat::flat_hash_map against std::unordered_map.
#include <sflat/flat_hash_map.hpp>

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <random>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#define CHECK(cond)                                                      \
  do {                                                                   \
    if (!(cond)) {                                                        \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      std::fflush(stdout);                                               \
      std::abort();                                                      \
    }                                                                    \
  } while (0)

template <typename K, typename V>
bool maps_equal(const sflat::flat_hash_map<K, V>& a,
                const std::unordered_map<K, V>& b) {
  if (a.size() != b.size()) return false;
  for (const auto& kv : a) {
    auto it = b.find(kv.first);
    if (it == b.end() || it->second != kv.second) return false;
  }
  return true;
}

// Randomized mixed workload vs std::unordered_map reference.
void test_random_ops() {
  using Map = sflat::flat_hash_map<uint64_t, uint64_t>;
  using Ref = std::unordered_map<uint64_t, uint64_t>;
  std::mt19937_64 rng(12345);
  for (int trial = 0; trial < 30; ++trial) {
    Map m;
    Ref r;
    // Random initial reserve to exercise growth paths.
    if (trial % 3 == 0) m.reserve(rng() % 5000);
    const int ops = 20000;
    for (int i = 0; i < ops; ++i) {
      const uint64_t k = rng() % 7000;
      const int op = rng() % 100;
      if (op < 55) {  // insert / emplace / operator[]
        const uint64_t v = rng();
        const int kind = rng() % 4;
        bool mine, refs;
        if (kind == 0) {
          auto pr = m.emplace(k, v);
          auto rr = r.emplace(k, v);
          mine = pr.second;
          refs = rr.second;
        } else if (kind == 1) {
          auto pr = m.insert(typename Map::value_type(k, v));
          auto rr = r.insert(typename Ref::value_type(k, v));
          mine = pr.second;
          refs = rr.second;
        } else if (kind == 2) {
          auto pr = m.try_emplace(k, v);
          mine = pr.second;
          auto it = r.find(k);
          refs = (it == r.end());
          if (refs) r.emplace(k, v);
        } else {
          auto it = r.find(k);
          refs = (it == r.end());
          m[k] = v;
          r[k] = v;
          mine = refs;
        }
        CHECK(mine == refs);
      } else if (op < 75) {  // find / count / contains
        auto it = m.find(k);
        auto jt = r.find(k);
        CHECK((it == m.end()) == (jt == r.end()));
        if (it != m.end()) CHECK(it->second == jt->second);
        CHECK(m.count(k) == r.count(k));
        CHECK(m.contains(k) == (jt != r.end()));
      } else if (op < 90) {  // erase
        const size_t n1 = m.erase(k);
        const size_t n2 = r.erase(k);
        CHECK(n1 == n2);
      } else if (op < 95) {  // iterator erase of a random element
        if (!m.empty()) {
          auto it = m.begin();
          std::advance(it, rng() % m.size());
          const uint64_t k2 = it->first;
          m.erase(it);
          r.erase(k2);
        }
      } else {  // insert_or_assign / at
        const uint64_t v = rng();
        m.insert_or_assign(k, v);
        r[k] = v;
        CHECK(m.at(k) == r.at(k));
      }
      if (i % 5000 == 4999) CHECK(maps_equal(m, r));
    }
    CHECK(maps_equal(m, r));
    // erase everything through iterators
    for (auto it = m.begin(); it != m.end();) {
      const uint64_t k = it->first;
      it = m.erase(it);
      r.erase(k);
    }
    CHECK(m.empty() && r.empty());
    CHECK(m.begin() == m.end());
  }
  std::printf("test_random_ops OK\n");
}

void test_string_keys() {
  using Map = sflat::flat_hash_map<std::string, std::string>;
  using Ref = std::unordered_map<std::string, std::string>;
  Map m;
  Ref r;
  std::mt19937_64 rng(999);
  std::vector<std::string> pool;
  for (int i = 0; i < 2000; ++i)
    pool.push_back("key_" + std::to_string(rng() % 500) + "_" +
                   std::to_string(i));
  for (int i = 0; i < 20000; ++i) {
    const std::string& k = pool[rng() % pool.size()];
    const int op = rng() % 10;
    if (op < 6) {
      std::string v = "v" + std::to_string(rng());
      CHECK(m.emplace(k, v).second == r.emplace(k, v).second);
    } else if (op < 8) {
      auto it = m.find(k);
      auto jt = r.find(k);
      CHECK((it == m.end()) == (jt == r.end()));
      if (it != m.end()) CHECK(it->second == jt->second);
    } else {
      CHECK(m.erase(k) == r.erase(k));
    }
  }
  CHECK(maps_equal(m, r));
  // iteration covers every element exactly once
  std::vector<std::string> keys;
  for (const auto& kv : m) keys.push_back(kv.first);
  std::sort(keys.begin(), keys.end());
  CHECK(keys.size() == m.size());
  CHECK(std::adjacent_find(keys.begin(), keys.end()) == keys.end());
  std::printf("test_string_keys OK\n");
}

void test_api_surface() {
  using Map = sflat::flat_hash_map<int, int>;
  // ctors
  Map m0;
  CHECK(m0.empty() && m0.size() == 0 && m0.begin() == m0.end());
  Map m1{{1, 10}, {2, 20}, {3, 30}};
  CHECK(m1.size() == 3 && m1.at(2) == 20);
  Map m2(m1);
  CHECK(m2 == m1);
  Map m3(std::move(m2));
  CHECK(m3 == m1 && m2.empty());
  Map m4;
  m4 = m1;
  CHECK(m4 == m1);
  Map m5;
  m5 = std::move(m4);
  CHECK(m5 == m1);
  m5 = {{9, 90}};
  CHECK(m5.size() == 1 && m5.at(9) == 90);
  // range ctor
  std::vector<std::pair<int, int>> vec{{5, 50}, {6, 60}};
  Map m6(vec.begin(), vec.end());
  CHECK(m6.size() == 2 && m6.at(6) == 60);
  // bucket_count ctor + reserve/rehash
  Map m7(1000);
  CHECK(m7.bucket_count() >= 1000);
  m7.reserve(5000);
  CHECK(m7.bucket_count() >= 5000);
  // operator[] / at throw
  Map m8;
  m8[7] = 70;
  CHECK(m8[7] == 70 && m8.at(7) == 70);
  bool threw = false;
  try {
    m8.at(12345);
  } catch (const std::out_of_range&) {
    threw = true;
  }
  CHECK(threw);
  // equal_range
  auto er = m8.equal_range(7);
  CHECK(er.first != m8.end() && er.first->second == 70);
  CHECK(m8.equal_range(4242).first == m8.end());
  // const access
  const Map& cm = m8;
  CHECK(cm.find(7)->second == 70 && cm.count(7) == 1 && cm.contains(7));
  // erase range [first, last) in iteration order
  Map m9{{1, 1}, {2, 2}, {3, 3}, {4, 4}};
  auto it1 = m9.begin(), it2 = m9.begin();
  std::advance(it2, 2);
  std::vector<int> erased_keys;
  for (auto it = it1; it != it2; ++it) erased_keys.push_back(it->first);
  m9.erase(it1, it2);
  CHECK(m9.size() == 4 - erased_keys.size());
  for (int k = 1; k <= 4; ++k) {
    const bool was_erased =
        std::find(erased_keys.begin(), erased_keys.end(), k) !=
        erased_keys.end();
    CHECK((m9.find(k) == m9.end()) == was_erased);
  }
  // swap
  Map a{{1, 1}}, b{{2, 2}, {3, 3}};
  a.swap(b);
  CHECK(a.size() == 2 && b.size() == 1);
  swap(a, b);
  CHECK(a.size() == 1 && b.size() == 2);
  CHECK(a != b);
  // bucket interface
  CHECK(b.bucket_count() > 0);
  const size_t bk = b.bucket(2);
  CHECK(bk < b.bucket_count());
  size_t local_count = 0;
  for (auto lit = b.begin(bk); lit != b.end(bk); ++lit) {
    CHECK(b.bucket(lit->first) == bk);
    ++local_count;
  }
  CHECK(local_count >= 1);  // key 2 itself hashes to bucket bk
  // bucket_size(n): open addressing, at most one element per slot n
  CHECK(b.bucket_size(bk) <= 1);
  // load factor / max load factor
  CHECK(m9.load_factor() >= 0.0);
  const double old = m9.max_load_factor();
  (void)old;
  m9.max_load_factor(0.5);
  CHECK(m9.max_load_factor() == 0.5);
  CHECK(m9.load_factor() <= 0.5 + 1e-9 || m9.empty());
  // hash_function / key_eq / get_allocator
  (void)m9.hash_function();
  (void)m9.key_eq();
  (void)m9.get_allocator();
  // clear keeps working
  m9.clear();
  CHECK(m9.empty());
  m9[42] = 1;
  CHECK(m9.size() == 1);
  // const_iterator conversion
  Map::iterator itw = m9.find(42);
  Map::const_iterator itc = itw;
  CHECK(itc->second == 1);
  std::printf("test_api_surface OK\n");
}

void test_growth_factor() {
  // Small tables grow 2x (see SFLAT_GROW_2X_BELOW); no step may be larger.
  // tests/growth_tiers.cpp checks the 1.5x and 1.25x tiers.
  sflat::flat_hash_map<uint64_t, uint64_t> m;
  size_t prev_slots = 0;
  double worst = 0.0;
  for (uint64_t i = 0; i < 300000; ++i) {
    m.emplace(i, i);
    const size_t c = m.bucket_count();
    if (c != prev_slots) {
      if (prev_slots >= 64) {
        const double f = static_cast<double>(c) / static_cast<double>(prev_slots);
        if (f > worst) worst = f;
        CHECK(f < 2.01);
      }
      prev_slots = c;
    }
  }
  std::printf("test_growth_factor OK (worst growth %.3f)\n", worst);
  // Memory bound: capacity stays at most 2x of ideal.
  const size_t cap = m.bucket_count();
  const double ideal = static_cast<double>(m.size()) / m.max_load_factor();
  CHECK(static_cast<double>(cap) < ideal * 2.01);
  // The capacity limit: fewer than 2^32 groups.
  CHECK(m.max_bucket_count() < (uint64_t{1} << 32) * 32);
  CHECK(m.max_size() < m.max_bucket_count());
  bool threw = false;
  try {
    m.reserve(m.max_size() + 1000);
  } catch (const std::length_error&) {
    threw = true;
  }
  CHECK(threw);
  CHECK(m.size() == 300000);
}

void test_sequential_keys() {
  // Identity hash on sequential keys: mixer must prevent clustering.
  sflat::flat_hash_map<uint64_t, uint64_t> m;
  const uint64_t n = 200000;
  m.reserve(n);
  for (uint64_t i = 0; i < n; ++i) m.emplace(i, i + 1);
  CHECK(m.size() == n);
  for (uint64_t i = 0; i < n; ++i) {
    auto it = m.find(i);
    CHECK(it != m.end() && it->second == i + 1);
  }
  // find-miss on keys just outside the range
  for (uint64_t i = n; i < n + 1000; ++i) CHECK(m.find(i) == m.end());
  std::printf("test_sequential_keys OK\n");
}

// Operations that run while an incremental migration is in progress.
// Each case fills a map until a growth migration has just started, so most
// elements are still in the old table.
void test_during_migration() {
  using Map = sflat::flat_hash_map<uint64_t, std::string>;
  auto fill = [](Map& m) {
    uint64_t k = 0;
    size_t bc = 0;
    for (;;) {
      m.emplace(k, std::to_string(k));
      ++k;
      if (m.size() > 5000 && bc && m.bucket_count() != bc) return k;
      bc = m.bucket_count();
    }
  };
  auto all_present = [](const Map& m, uint64_t n) {
    if (m.size() != n) return false;
    for (uint64_t i = 0; i < n; ++i) {
      auto it = m.find(i);
      if (it == m.end() || it->second != std::to_string(i)) return false;
    }
    return true;
  };
  {  // copy construction and copy assignment
    Map m;
    const uint64_t n = fill(m);
    Map c(m);
    CHECK(all_present(c, n));
    CHECK(static_cast<size_t>(std::distance(c.begin(), c.end())) == n);
    Map m2;
    fill(m2);
    Map d;
    d = m2;
    CHECK(all_present(d, n));
  }
  {  // move construction and move assignment
    Map m;
    const uint64_t n = fill(m);
    Map c(std::move(m));
    CHECK(all_present(c, n));
    Map m2;
    fill(m2);
    Map d;
    d = std::move(m2);
    CHECK(all_present(d, n));
  }
  {  // clear must drop the old table too
    Map m;
    const uint64_t n = fill(m);
    m.clear();
    CHECK(m.empty());
    for (uint64_t i = 0; i < n; ++i) CHECK(m.find(i) == m.end());
    CHECK(m.begin() == m.end());
  }
  {  // cbegin must see elements of the old table
    Map m;
    const uint64_t n = fill(m);
    size_t cnt = 0;
    for (auto it = m.cbegin(); it != m.cend(); ++it) ++cnt;
    CHECK(cnt == n);
  }
  {  // insert_or_assign(key_type&&) must not duplicate old-table keys
    Map m;
    const uint64_t n = fill(m);
    for (uint64_t i = 0; i < 100; ++i)
      CHECK(!m.insert_or_assign(uint64_t(i), std::string("x")).second);
    CHECK(m.size() == n);
    for (uint64_t i = 0; i < 100; ++i) CHECK(m.at(i) == "x");
  }
  std::printf("test_during_migration OK\n");
}

// Large tables (arrays >= 2 MiB use mmap, THP and page release on Linux):
// mixed operations against std::unordered_map across several growths, with
// checks while a migration is in progress.
void test_large_mixed() {
  using Map = sflat::flat_hash_map<uint64_t, uint64_t>;
  std::mt19937_64 rng(777);
  Map m;
  std::unordered_map<uint64_t, uint64_t> r;
  size_t migrating_checks = 0;
  size_t last_bc = 0;
  for (int i = 0; i < 1500000; ++i) {
    const uint64_t k = rng() % 1200000;
    const int op = rng() % 10;
    if (op < 6) {
      const uint64_t v = rng();
      CHECK(m.insert_or_assign(k, v).second == !r.count(k));
      r[k] = v;
    } else if (op < 8) {
      CHECK(m.erase(k) == r.erase(k));
    } else {
      auto it = m.find(k);
      auto rit = r.find(k);
      CHECK((it == m.end()) == (rit == r.end()));
      if (rit != r.end()) CHECK(it->second == rit->second);
    }
    // Shortly after each growth, compare everything and erase while
    // iterating (the migration is still in progress at this point).
    if (m.bucket_count() != last_bc) {
      last_bc = m.bucket_count();
      if (m.size() > 100000) {
        for (int j = 0; j < 50; ++j) {  // let the migration run a bit
          const uint64_t kk = rng() % 1200000;
          m[kk] = kk;
          r[kk] = kk;
        }
        CHECK(m.size() == r.size());
        size_t n = 0;
        for (auto it = m.begin(); it != m.end();) {
          auto rit = r.find(it->first);
          CHECK(rit != r.end() && rit->second == it->second);
          ++n;
          if (it->first % 97 == 0) {
            r.erase(it->first);
            it = m.erase(it);
          } else {
            ++it;
          }
        }
        CHECK(m.size() == r.size());
        ++migrating_checks;
      }
    }
  }
  CHECK(maps_equal(m, r));
  CHECK(migrating_checks > 0);
  Map c(m);
  CHECK(maps_equal(c, r));
  m.optimize();
  CHECK(maps_equal(m, r));
  std::printf("test_large_mixed OK (%zu checks during growth)\n",
              migrating_checks);
}

int main() {
  test_api_surface();
  test_random_ops();
  test_string_keys();
  test_growth_factor();
  test_sequential_keys();
  test_during_migration();
  test_large_mixed();
  std::printf("ALL CORRECTNESS TESTS PASSED\n");
  return 0;
}
