// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jian-Cheng Lin
// Correctness tests for sflat::flat_hash_set against std::unordered_set.
// The table logic is shared with flat_hash_map (tests/correctness.cpp);
// these tests cover the set interface and the same table paths: growth,
// migration, tombstones, large tables, user allocators and self-aliasing.
#include <sflat/flat_hash_set.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <iterator>
#include <random>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_set>
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

template <typename K, typename... Rest>
bool sets_equal(const sflat::flat_hash_set<K, Rest...>& a,
                const std::unordered_set<K>& b) {
  if (a.size() != b.size()) return false;
  size_t n = 0;
  for (const auto& k : a) {
    if (!b.count(k)) return false;
    ++n;
  }
  return n == b.size();  // iteration visits every element exactly once
}

// Interface: types, constness of elements, the calls of std::unordered_set.
void test_api_surface() {
  using Set = sflat::flat_hash_set<uint64_t>;
  static_assert(std::is_same<Set::value_type, uint64_t>::value, "");
  static_assert(std::is_same<Set::key_type, uint64_t>::value, "");
  // Elements are immutable: changing one would break the table.
  static_assert(std::is_same<decltype(*std::declval<Set::iterator>()),
                             const uint64_t&>::value, "");
  static_assert(std::is_same<decltype(*std::declval<Set::const_iterator>()),
                             const uint64_t&>::value, "");

  Set s{1, 2, 3, 2};
  CHECK(s.size() == 3);
  CHECK(s.contains(2) && !s.contains(4));
  CHECK(s.count(3) == 1 && s.count(9) == 0);

  auto r = s.insert(4);
  CHECK(r.second && *r.first == 4);
  r = s.insert(4);
  CHECK(!r.second && *r.first == 4);
  const uint64_t five = 5;
  CHECK(s.insert(five).second);
  CHECK(s.emplace(6).second);
  CHECK(!s.emplace(uint64_t{6}).second);
  CHECK(*s.emplace_hint(s.cbegin(), 7) == 7);
  CHECK(*s.insert(s.cbegin(), 8) == 8);
  std::vector<uint64_t> more{9, 10, 1};
  s.insert(more.begin(), more.end());
  s.insert({11, 12});
  CHECK(s.size() == 12);

  CHECK(s.erase(12) == 1 && s.erase(12) == 0);
  auto it = s.find(11);
  CHECK(it != s.end() && *it == 11);
  s.erase(it);
  CHECK(!s.contains(11));
  auto er = s.equal_range(10);
  CHECK(std::distance(er.first, er.second) == 1 && *er.first == 10);
  er = s.equal_range(99);
  CHECK(er.first == er.second);

  const Set& cs = s;
  CHECK(cs.find(1) != cs.end() && *cs.find(1) == 1);
  Set::const_iterator ci = s.begin();  // iterator -> const_iterator
  CHECK(ci == s.cbegin());

  // Copy, compare, swap, assignment from a list.
  Set c = s;
  CHECK(c == s);
  c.insert(100);
  CHECK(c != s);
  swap(c, s);
  CHECK(s.contains(100) && !c.contains(100));
  c = {1, 2};
  CHECK(c.size() == 2);

  // Buckets and local iterators.
  CHECK(s.bucket_count() >= s.size());
  const auto b = s.bucket(4);
  size_t in_bucket = 0;
  for (auto lit = s.begin(b); lit != s.end(b); ++lit) {
    CHECK(s.bucket(*lit) == b);
    ++in_bucket;
  }
  CHECK(in_bucket >= 1);  // 4 itself

  // Capacity calls.
  s.reserve(1000);
  CHECK(s.bucket_count() >= 1000);
  s.shrink_to_fit();
  s.optimize();
  s.max_load_factor(0.5);
  CHECK(s.load_factor() <= 0.5 + 1e-9);
  CHECK(s.contains(100));
  s.clear();
  CHECK(s.empty() && s.begin() == s.end());
  std::printf("test_api_surface OK\n");
}

void test_random_ops() {
  using Set = sflat::flat_hash_set<uint64_t>;
  std::mt19937_64 rng(12345);
  for (int trial = 0; trial < 30; ++trial) {
    Set s;
    std::unordered_set<uint64_t> r;
    if (trial % 3 == 0) s.reserve(rng() % 5000);
    for (int i = 0; i < 20000; ++i) {
      const uint64_t k = rng() % 7000;
      const int op = rng() % 100;
      if (op < 55) {
        const bool mine = (rng() % 2) ? s.insert(k).second : s.emplace(k).second;
        CHECK(mine == r.insert(k).second);
      } else if (op < 75) {
        CHECK((s.find(k) == s.end()) == (r.find(k) == r.end()));
        CHECK(s.count(k) == r.count(k));
      } else if (op < 92) {
        CHECK(s.erase(k) == r.erase(k));
      } else if (!s.empty()) {  // iterator erase of a random element
        auto it = s.begin();
        std::advance(it, rng() % s.size());
        const uint64_t k2 = *it;
        s.erase(it);
        r.erase(k2);
      }
      if (i % 5000 == 4999) CHECK(sets_equal(s, r));
    }
    CHECK(sets_equal(s, r));
    for (auto it = s.begin(); it != s.end();) {
      r.erase(*it);
      it = s.erase(it);
    }
    CHECK(s.empty() && r.empty() && s.begin() == s.end());
  }
  std::printf("test_random_ops OK\n");
}

// Non-trivial keys: construction, move during growth and destruction.
void test_string_keys() {
  using Set = sflat::flat_hash_set<std::string>;
  std::mt19937_64 rng(99);
  Set s;
  std::unordered_set<std::string> r;
  for (int i = 0; i < 200000; ++i) {
    std::string k = "key_long_enough_to_skip_sso_" + std::to_string(rng() % 60000);
    if (rng() % 4 == 0) {
      CHECK(s.erase(k) == r.erase(k));
    } else if (rng() % 2) {
      CHECK(s.insert(k).second == r.insert(k).second);
    } else {
      std::string copy = k;
      CHECK(s.insert(std::move(copy)).second == r.insert(k).second);
    }
  }
  CHECK(sets_equal(s, r));
  Set moved(std::move(s));
  CHECK(sets_equal(moved, r));
  std::printf("test_string_keys OK\n");
}

// Growth migrates in steps; copies, moves, clear, iteration and inserts of
// existing keys must see the old table too.
void test_during_migration() {
  using Set = sflat::flat_hash_set<uint64_t>;
  auto fill = [](Set& s) {
    uint64_t k = 0;
    size_t bc = 0;
    for (;;) {
      s.insert(k++);
      if (s.size() > 5000 && bc && s.bucket_count() != bc) return k;
      bc = s.bucket_count();
    }
  };
  auto all_present = [](const Set& s, uint64_t n) {
    if (s.size() != n) return false;
    for (uint64_t i = 0; i < n; ++i)
      if (!s.contains(i)) return false;
    return true;
  };
  {
    Set s;
    const uint64_t n = fill(s);
    Set c(s);
    CHECK(all_present(c, n));
    CHECK(static_cast<uint64_t>(std::distance(c.begin(), c.end())) == n);
    Set d;
    d = s;
    CHECK(all_present(d, n));
    Set m(std::move(s));
    CHECK(all_present(m, n));
  }
  {
    Set s;
    const uint64_t n = fill(s);
    s.clear();
    CHECK(s.empty() && s.begin() == s.end());
    for (uint64_t i = 0; i < n; ++i) CHECK(!s.contains(i));
  }
  {  // keys of the old table must not be inserted twice
    Set s;
    const uint64_t n = fill(s);
    for (uint64_t i = 0; i < 100; ++i) CHECK(!s.insert(i).second);
    CHECK(s.size() == n);
    size_t cnt = 0;
    for (auto it = s.cbegin(); it != s.cend(); ++it) ++cnt;
    CHECK(cnt == n);
  }
  std::printf("test_during_migration OK\n");
}

// Large tables (mmap, THP and page release on Linux) across several growths,
// with erase-while-iterating during a migration.
void test_large_mixed() {
  using Set = sflat::flat_hash_set<uint64_t>;
  std::mt19937_64 rng(777);
  Set s;
  std::unordered_set<uint64_t> r;
  size_t migrating_checks = 0;
  size_t last_bc = 0;
  for (int i = 0; i < 1500000; ++i) {
    const uint64_t k = rng() % 1200000;
    const int op = rng() % 10;
    if (op < 6) {
      CHECK(s.insert(k).second == r.insert(k).second);
    } else if (op < 8) {
      CHECK(s.erase(k) == r.erase(k));
    } else {
      CHECK(s.contains(k) == (r.count(k) == 1));
    }
    if (s.bucket_count() != last_bc) {
      last_bc = s.bucket_count();
      if (s.size() > 100000) {
        for (int j = 0; j < 50; ++j) {  // let the migration run a bit
          const uint64_t kk = rng() % 1200000;
          s.insert(kk);
          r.insert(kk);
        }
        for (auto it = s.begin(); it != s.end();) {
          CHECK(r.count(*it));
          if (*it % 97 == 0) {
            r.erase(*it);
            it = s.erase(it);
          } else {
            ++it;
          }
        }
        CHECK(s.size() == r.size());
        ++migrating_checks;
      }
    }
  }
  CHECK(sets_equal(s, r));
  CHECK(migrating_checks > 0);
  Set c(s);
  CHECK(sets_equal(c, r));
  s.optimize();
  CHECK(sets_equal(s, r));
  std::printf("test_large_mixed OK (%zu checks during growth)\n", migrating_checks);
}

// A user allocator is used for all arrays; live bytes must return to zero.
static long long g_live_bytes = 0;
template <typename T>
struct CountingAlloc {
  using value_type = T;
  static constexpr size_t kSkew = alignof(T) == 1 ? 1 : 0;
  CountingAlloc() = default;
  template <typename U>
  CountingAlloc(const CountingAlloc<U>&) noexcept {}
  T* allocate(size_t n) {
    g_live_bytes += static_cast<long long>(n * sizeof(T));
    char* p = std::allocator<char>().allocate(n * sizeof(T) + kSkew);
    return reinterpret_cast<T*>(p + kSkew);
  }
  void deallocate(T* p, size_t n) noexcept {
    g_live_bytes -= static_cast<long long>(n * sizeof(T));
    std::allocator<char>().deallocate(reinterpret_cast<char*>(p) - kSkew,
                                      n * sizeof(T) + kSkew);
  }
  template <typename U>
  bool operator==(const CountingAlloc<U>&) const noexcept { return true; }
  template <typename U>
  bool operator!=(const CountingAlloc<U>&) const noexcept { return false; }
};

void test_custom_allocator() {
  {
    using Set = sflat::flat_hash_set<uint64_t, std::hash<uint64_t>,
                                     std::equal_to<uint64_t>,
                                     CountingAlloc<uint64_t>>;
    Set s;
    std::unordered_set<uint64_t> r;
    std::mt19937_64 rng(7);
    for (int i = 0; i < 200000; ++i) {
      const uint64_t k = rng() % 150000;
      if (rng() % 4 == 0) CHECK(s.erase(k) == r.erase(k));
      else CHECK(s.insert(k).second == r.insert(k).second);
    }
    CHECK(sets_equal(s, r));
    Set c(s);
    CHECK(sets_equal(c, r));
    Set mv(std::move(c));
    CHECK(sets_equal(mv, r));
    CHECK(g_live_bytes > 0);
  }
  CHECK(g_live_bytes == 0);
  std::printf("test_custom_allocator OK\n");
}

// The argument may be an element of the set itself (s.insert(*it)), also at
// the growth point and during migration, which move elements and free the
// old table. Strings longer than the SSO buffer let ASan see bad reads.
void test_self_aliasing() {
  using Set = sflat::flat_hash_set<std::string>;
  auto key = [](int i) { return "key_long_enough_to_skip_sso_" + std::to_string(i); };
  for (int n = 1; n < 600; ++n) {
    for (int op = 0; op < 2; ++op) {
      Set s;
      for (int i = 0; i < n; ++i) {
        s.insert(key(i));
        const std::string& e = *s.find(key(i / 2));
        const size_t before = s.size();
        if (op == 0) CHECK(!s.insert(e).second);
        else CHECK(!s.emplace(e).second);
        CHECK(s.size() == before);
      }
      for (int i = 0; i < n; ++i) CHECK(s.contains(key(i)));
    }
  }
  std::printf("test_self_aliasing OK\n");
}

// The point of a separate set: a slot holds only the key.
void test_memory_per_slot() {
  sflat::flat_hash_set<uint64_t> s;
  s.reserve(1 << 20);
  for (uint64_t i = 0; i < (1 << 20); ++i) s.insert(i * 0x9E3779B97F4A7C15ULL);
  // Slot (8) + control byte (1) + overflow word share; at most 1/0.875 slack.
  const double bytes = s.bucket_count() * (8.0 + 1.0 + 4.0 / 16) / s.size();
  CHECK(bytes < 11.0);
  std::printf("test_memory_per_slot OK (%.1f bytes/entry)\n", bytes);
}

// Erase one key and insert a new one, many times, at a fixed size (see
// test_churn_fixed_size in tests/correctness.cpp). std::string keys longer
// than the SSO buffer exercise the moves of the in-place rebuild.
void test_churn_fixed_size() {
  std::mt19937_64 rng(2026);
  for (size_t n : {30u, 900u, 1500u, 1790u, 3000u, 14000u, 28600u}) {
    sflat::flat_hash_set<std::string> s;
    std::unordered_set<std::string> ref;
    std::vector<std::string> live;
    auto key = [&] { return std::string(30, 'k') + std::to_string(rng()); };
    while (live.size() < n) {
      std::string k = key();
      if (ref.insert(k).second) {
        s.insert(k);
        live.push_back(k);
      }
    }
    const size_t buckets = s.bucket_count();
    for (size_t i = 0; i < 20 * n + 20000; ++i) {
      const size_t at = rng() % live.size();
      CHECK(s.erase(live[at]) == 1);
      ref.erase(live[at]);
      std::string k;
      do {
        k = key();
      } while (ref.count(k));
      CHECK(s.insert(k).second);
      ref.insert(k);
      live[at] = k;
      if (i % 97 == 0) {
        CHECK(s.count(live[rng() % live.size()]) == 1);
        CHECK(s.count("absent" + std::to_string(rng())) == 0);
      }
    }
    CHECK(s.bucket_count() == buckets);
    CHECK(sets_equal(s, ref));
  }
  std::printf("test_churn_fixed_size OK\n");
}

// Erasing other elements during an iteration (see the map test of the same
// name): ++ must not visit an erased element.
void test_erase_others_while_iterating() {
  for (size_t n : {10u, 100u, 5000u}) {
    sflat::flat_hash_set<uint64_t> s;
    std::unordered_set<uint64_t> alive, visited;
    for (uint64_t i = 0; i < n; ++i) {
      s.insert(i * 7919);
      alive.insert(i * 7919);
    }
    size_t step = 0;
    for (auto it = s.begin(); it != s.end(); ++it, ++step) {
      CHECK(alive.count(*it) == 1);
      CHECK(visited.insert(*it).second);
      if (step % 4 == 0) {
        std::vector<uint64_t> drop;
        for (uint64_t k : alive)
          if (k != *it && !visited.count(k) && (k / 7919) % 3 == step % 3)
            drop.push_back(k);
        for (uint64_t k : drop) {
          CHECK(s.erase(k) == 1);
          alive.erase(k);
        }
      }
    }
    for (uint64_t k : alive) CHECK(visited.count(k) == 1);
    CHECK(s.size() == alive.size());
  }
  std::printf("test_erase_others_while_iterating OK\n");
}

// Inserting while iterating another table must not take quadratic time. Each
// table has its own seed, so iteration order is not the home order of another
// table. The seed moves with the storage. (1M keys took 14 s without a seed.)
void test_iterate_and_insert() {
#if SFLAT_NO_SEED
  std::printf("test_iterate_and_insert skipped (SFLAT_NO_SEED)\n");
  return;
#endif
  using M = sflat::flat_hash_set<uint64_t>;
  std::mt19937_64 r(42);
  M a;
  for (int i = 0; i < 1000000; ++i) {
    M& m = a;
    m.insert(r());
  }
  M b;
  const auto start = std::chrono::steady_clock::now();
  for (const auto& k : a) b.insert(k);
  const std::chrono::duration<double> took =
      std::chrono::steady_clock::now() - start;
  CHECK(took.count() < 5.0);
  CHECK(b.size() == a.size());
  CHECK(*a.begin() != *b.begin());  // different seeds, different order

  const auto all_found = [](const M& x, uint64_t n) {
    for (uint64_t i = 0; i < n; ++i)
      if (x.find(i) == x.end()) return false;
    return x.size() == n;
  };
  const uint64_t n = 1000;
  const auto filled = [&] {
    M x;
    for (uint64_t i = 0; i < n; ++i) x.insert(i);
    return x;
  };
  M moved(filled());
  CHECK(all_found(moved, n));
  M from = filled();
  M to(std::move(from));
  CHECK(all_found(to, n));
  // A moved-from table takes a new seed and works again.
  from.clear();
  for (uint64_t i = 0; i < n; ++i) {
    M& x = from;
    x.insert(i);
  }
  CHECK(all_found(from, n));
  M assigned;
  assigned = std::move(from);
  CHECK(all_found(assigned, n));
  M copy_assigned;
  copy_assigned = assigned;
  CHECK(all_found(copy_assigned, n));
  M copied(assigned);
  CHECK(all_found(copied, n));
  M s1 = filled(), s2;
  s1.swap(s2);
  CHECK(s1.empty() && all_found(s2, n));
  std::vector<M> v(1);
  for (uint64_t i = 0; i < n; ++i) {
    M& x = v[0];
    x.insert(i);
  }
  v.resize(100);
  CHECK(all_found(v[0], n));
  // The first insert computes the hash before the seed is taken.
  M one;
  auto first_insert = one.find(7);
  CHECK(first_insert == one.end());
  {
    M& x = one;
    uint64_t i = 7;
    x.insert(i);
  }
  CHECK(one.find(7) != one.end());
  M reserved;
  reserved.reserve(100);
  {
    M& x = reserved;
    uint64_t i = 7;
    x.insert(i);
  }
  CHECK(reserved.find(7) != reserved.end());
  std::printf("test_iterate_and_insert OK\n");
}

// Safety net of the seed, as in tests/correctness.cpp: b = a, then clear
// and shrink, so b keeps the multiplier of a; the keys of a then pile up in
// b until a long probe gives b a new seed. String keys, with erases.
void test_safety_net() {
#if SFLAT_NO_SEED
  std::printf("test_safety_net skipped (SFLAT_NO_SEED)\n");
  return;
#endif
  using S = sflat::flat_hash_set<std::string>;
  const uint64_t n = 500000;
  S a;
  for (uint64_t i = 0; i < n; ++i) a.insert(std::to_string(i * 7));
  S b = a;
  b.clear();
  b.rehash(0);
  std::vector<std::string> erased;
  std::string prev;
  uint64_t k = 0;
  const auto start = std::chrono::steady_clock::now();
  for (const auto& x : a) {
    b.insert(x);
    if (++k % 64 == 0) {
      b.erase(prev);
      erased.push_back(prev);
    }
    prev = x;
  }
  const std::chrono::duration<double> took =
      std::chrono::steady_clock::now() - start;
  CHECK(took.count() < 1.0);
  CHECK(b.size() == n - erased.size());
  CHECK(static_cast<size_t>(std::distance(b.begin(), b.end())) == b.size());
  for (const auto& x : erased) CHECK(b.find(x) == b.end());
  size_t found = 0;
  for (const auto& x : a) found += b.count(x);
  CHECK(found == b.size());
  std::printf("test_safety_net OK (%.3f s)\n", took.count());
}

// A single-pass iterator over a vector (see tests/correctness.cpp).
template <typename T>
struct InputOnly {
  using iterator_category = std::input_iterator_tag;
  using value_type = T;
  using difference_type = std::ptrdiff_t;
  using pointer = const T*;
  using reference = const T&;
  const T* p;
  reference operator*() const { return *p; }
  InputOnly& operator++() { ++p; return *this; }
  InputOnly operator++(int) { InputOnly t = *this; ++p; return t; }
  bool operator==(const InputOnly& o) const { return p == o.p; }
  bool operator!=(const InputOnly& o) const { return p != o.p; }
};

// insert(first, last) makes room first for forward iterators, as in the map.
void test_range_insert() {
  using S = sflat::flat_hash_set<uint64_t>;
  const uint64_t n = 500000;
  S a;
  for (uint64_t i = 0; i < n; ++i) a.insert(i * 3);
  const auto start = std::chrono::steady_clock::now();
  S b(a.begin(), a.end());
  const std::chrono::duration<double> took =
      std::chrono::steady_clock::now() - start;
  CHECK(took.count() < 0.5);
  CHECK(b == a);
  const size_t bc = b.bucket_count();
  b.insert(a.begin(), a.end());
  b.insert(b.begin(), b.end());
  CHECK(b.bucket_count() == bc && b == a);
  S r;
  r.reserve(100000);
  const size_t rbc = r.bucket_count();
  const std::vector<uint64_t> few = {1, 2, 3};
  r.insert(few.begin(), few.end());
  CHECK(r.bucket_count() == rbc && r.size() == 3);
  std::vector<uint64_t> v;
  for (uint64_t i = 0; i < 1000; ++i) v.push_back(i % 700);
  S in;
  in.insert(InputOnly<uint64_t>{v.data()}, InputOnly<uint64_t>{v.data() + v.size()});
  CHECK(in.size() == 700);
  for (uint64_t k = 0; k < 700; ++k) CHECK(in.count(k) == 1);
  std::printf("test_range_insert OK (%.3f s)\n", took.count());
}

int main() {
  test_api_surface();
  test_random_ops();
  test_string_keys();
  test_during_migration();
  test_large_mixed();
  test_custom_allocator();
  test_self_aliasing();
  test_memory_per_slot();
  test_churn_fixed_size();
  test_erase_others_while_iterating();
  test_iterate_and_insert();
  test_safety_net();
  test_range_insert();
  std::printf("ALL SET TESTS PASSED\n");
  return 0;
}
