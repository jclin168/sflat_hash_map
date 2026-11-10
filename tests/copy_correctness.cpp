// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jian-Cheng Lin
#include <sflat/flat_hash_map.hpp>
#include <sflat/flat_hash_set.hpp>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <type_traits>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); std::abort(); } } while (0)

struct CountingHash {
  static size_t calls;
  uint64_t salt = 0;
  size_t operator()(uint64_t key) const noexcept { ++calls; return std::hash<uint64_t>{}(key ^ salt); }
};
size_t CountingHash::calls = 0;
struct Equality {
  unsigned state = 0;
  bool operator()(uint64_t a, uint64_t b) const noexcept { return a == b; }
};

template<class C> void put(C& c, uint64_t key) {
  if constexpr (std::is_same<typename C::key_type, typename C::value_type>::value) c.insert(key);
  else c.emplace(key, key * 3);
}
template<class C> uint64_t first_key(const C& c) {
  if constexpr (std::is_same<typename C::key_type, typename C::value_type>::value) return *c.begin();
  else return c.begin()->first;
}

template<class C> void test_reuse() {
  C source(0, CountingHash{7}, Equality{11});
  source.reserve(20000);
  for (uint64_t i = 0; i < 20000; ++i) put(source, i);
  for (uint64_t i = 0; i < 20000; i += 20) source.erase(i);
  C target = source;
  const uint64_t key = first_key(target);
  const uintptr_t address = reinterpret_cast<uintptr_t>(&*target.find(key));
  CountingHash::calls = 0;
  target = source;
  CHECK(CountingHash::calls == 0 || SFLAT_COPY_SLACK < 1.1);
  if (SFLAT_COPY_SLACK >= 1.1)
    CHECK(reinterpret_cast<uintptr_t>(&*target.find(key)) == address);
  CHECK(target == source);
  CHECK(target.hash_function().salt == 7 && target.key_eq().state == 11);
  target.erase(key);
  CHECK(source.find(key) != source.end());
  target = source;
  CHECK(target == source);
  target = target;
  CHECK(target == source);

  C different(0, CountingHash{93}, Equality{42});
  different.reserve(20000);
  for (uint64_t i = 30000; i < 40000; ++i) put(different, i);
  different.max_load_factor(0.95);
  different = source;
  CHECK(different == source);
  CHECK(different.hash_function().salt == 7 && different.key_eq().state == 11);
  CHECK(different.max_load_factor() == source.max_load_factor());
  for (uint64_t i = 0; i < 20000; ++i)
    CHECK((different.find(i) != different.end()) == (i % 20 != 0));
  for (uint64_t i = 40000; i < 41000; ++i) put(different, i);
  CHECK(different.size() == source.size() + 1000);
  C empty;
  empty.reserve(20000);
  different = empty;
  CHECK(different.empty() && different.bucket_count() == 0);
}

template<class C> void test_copy_then_update() {
  C original;
  for (uint64_t i = 0; i < 1000000; ++i) put(original, i);
  C source = original;
  const size_t capacity = source.bucket_count();
  for (uint64_t i = 0; i < 200; ++i) {
    CountingHash::calls = 0;
    C copy = source;
    if (SFLAT_COPY_SLACK >= 1.25) CHECK(CountingHash::calls == 0);
    CHECK(copy.size() == 1000000 + i);
    CHECK(copy.find(500000) != copy.end());
    put(copy, 2000000);
    CHECK(source.find(2000000) == source.end());
    put(source, 1000000 + i);
  }
  if (SFLAT_COPY_SLACK >= 1.25) CHECK(source.bucket_count() == capacity);
  CHECK(original.size() == 1000000 && original.find(1000000) == original.end());
}

template<class C> void test_growth_copy() {
  C growing;
  size_t previous = 0;
  uint64_t key = 0;
  for (; key < 100000; ++key) {
    put(growing, key);
    if (key > 5000 && growing.bucket_count() != previous) break;
    previous = growing.bucket_count();
  }
  const C copied = growing;
  CHECK(copied == growing);
  C stable;
  stable.reserve(static_cast<size_t>(growing.bucket_count() * growing.max_load_factor()));
  for (uint64_t i = 0; i < key + 1000; ++i) put(stable, i);
  growing = stable;
  CHECK(growing == stable);
}

struct ThrowingValue {
  static int live;
  static int remaining;
  uint64_t value;
  explicit ThrowingValue(uint64_t v = 0) : value(v) { ++live; }
  ThrowingValue(const ThrowingValue& v) : value(v.value) {
    if (remaining == 0) throw std::runtime_error("copy");
    if (remaining > 0) --remaining;
    ++live;
  }
  ThrowingValue(ThrowingValue&& v) noexcept : value(v.value) { ++live; }
  ~ThrowingValue() { --live; }
};
int ThrowingValue::live = 0;
int ThrowingValue::remaining = -1;
void test_assignment_failure() {
  using M = sflat::flat_hash_map<uint64_t, ThrowingValue>;
  M source, target;
  source.reserve(1000);
  target.reserve(1000);
  for (uint64_t i = 0; i < 1000; ++i) source.try_emplace(i, i);
  target.try_emplace(5000, 17);
  const int before = ThrowingValue::live;
  for (int remaining : {0, 1, 500, 999}) {
    ThrowingValue::remaining = remaining;
    bool threw = false;
    try { target = source; } catch (const std::runtime_error&) { threw = true; }
    CHECK(threw && ThrowingValue::live == before);
    CHECK(target.size() == 1 && target.at(5000).value == 17);
    CHECK(source.size() == 1000);
  }
  ThrowingValue::remaining = -1;
  target = source;
  CHECK(target.size() == source.size());
}

int main() {
  using M = sflat::flat_hash_map<uint64_t, uint64_t, CountingHash, Equality>;
  using S = sflat::flat_hash_set<uint64_t, CountingHash, Equality>;
  test_reuse<M>(); test_reuse<S>();
  test_copy_then_update<M>(); test_copy_then_update<S>();
  test_growth_copy<M>(); test_growth_copy<S>();
  test_assignment_failure();
  CHECK(ThrowingValue::live == 0);
  std::puts("copy correctness PASS");
}
