// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jian-Cheng Lin
// Unit tests of the std::unordered_set interface of sflat::flat_hash_set.
//
// Every test is a template that runs twice: on std::unordered_set, which
// shows that the expectation is standard behaviour, and on
// sflat::flat_hash_set. Not covered, because sflat does not offer them:
// node handles (extract, insert(node_type&&), node_type,
// insert_return_type), merge, and the reference stability of
// std::unordered_set across a rehash (a flat table moves its elements).
#include <sflat/flat_hash_set.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <memory>
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

template <typename S>
std::vector<typename S::key_type> sorted(const S& s) {
  std::vector<typename S::key_type> v(s.begin(), s.end());
  std::sort(v.begin(), v.end());
  return v;
}

struct SeededHash {
  size_t seed = 0;
  SeededHash() = default;
  explicit SeededHash(size_t s) : seed(s) {}
  size_t operator()(int k) const { return std::hash<int>()(k) ^ seed; }
};
struct ModEqual {  // keys equal modulo 1000
  bool operator()(int a, int b) const { return a % 1000 == b % 1000; }
};
struct ModHash {
  size_t operator()(int k) const { return std::hash<int>()(k % 1000); }
};

// ---------------------------------------------------------------- types
template <template <typename...> class Set>
void test_member_types() {
  using S = Set<std::string>;
  static_assert(std::is_same<typename S::key_type, std::string>::value, "");
  static_assert(std::is_same<typename S::value_type, std::string>::value, "");
  static_assert(std::is_same<typename S::size_type, size_t>::value, "");
  static_assert(std::is_same<typename S::difference_type, std::ptrdiff_t>::value, "");
  static_assert(std::is_same<typename S::hasher, std::hash<std::string>>::value, "");
  static_assert(std::is_same<typename S::key_equal, std::equal_to<std::string>>::value, "");
  static_assert(std::is_same<typename S::allocator_type, std::allocator<std::string>>::value, "");
  static_assert(std::is_same<typename S::reference, std::string&>::value, "");
  static_assert(std::is_same<typename S::const_reference, const std::string&>::value, "");
  static_assert(std::is_same<typename S::pointer, std::string*>::value, "");
  static_assert(std::is_same<typename S::const_pointer, const std::string*>::value, "");
  using It = typename S::iterator;
  using CIt = typename S::const_iterator;
  static_assert(std::is_base_of<std::forward_iterator_tag,
                                typename std::iterator_traits<It>::iterator_category>::value, "");
  static_assert(std::is_same<typename std::iterator_traits<It>::value_type, std::string>::value, "");
  // Set elements are constant through both iterator types.
  static_assert(std::is_same<decltype(*std::declval<It>()), const std::string&>::value, "");
  static_assert(std::is_same<decltype(*std::declval<CIt>()), const std::string&>::value, "");
  static_assert(std::is_convertible<It, CIt>::value, "");
  static_assert(std::is_default_constructible<It>::value, "");
  static_assert(std::is_default_constructible<typename S::local_iterator>::value, "");
  static_assert(std::is_same<decltype(*std::declval<typename S::local_iterator>()),
                             const std::string&>::value, "");
}

// ---------------------------------------------------------- construction
template <template <typename...> class Set>
void test_constructors() {
  using S = Set<int>;
  using A = typename S::allocator_type;
  const std::vector<int> src{1, 2, 3, 2};

  S a;
  CHECK(a.empty() && a.size() == 0 && a.begin() == a.end());
  S b(100);
  CHECK(b.empty() && b.bucket_count() >= 100);
  S c(10, std::hash<int>());
  S d(10, std::hash<int>(), std::equal_to<int>());
  S e(10, std::hash<int>(), std::equal_to<int>(), A());
  S f(10, A());
  S g(10, std::hash<int>(), A());
  S h{A()};
  CHECK(c.empty() && d.empty() && e.empty() && f.empty() && g.empty() && h.empty());

  S r1(src.begin(), src.end());
  CHECK(r1.size() == 3);
  S r2(src.begin(), src.end(), 10);
  S r3(src.begin(), src.end(), 10, std::hash<int>());
  S r4(src.begin(), src.end(), 10, std::hash<int>(), std::equal_to<int>(), A());
  S r5(src.begin(), src.end(), 10, A());
  S r6(src.begin(), src.end(), 10, std::hash<int>(), A());
  CHECK(sorted(r2) == sorted(r1) && sorted(r3) == sorted(r1) && sorted(r4) == sorted(r1));
  CHECK(sorted(r5) == sorted(r1) && sorted(r6) == sorted(r1));

  S l1{1, 2, 3, 2};
  S l2({1, 2, 3}, 10);
  S l3({1, 2, 3}, 10, std::hash<int>());
  S l4({1, 2, 3}, 10, A());
  S l5({1, 2, 3}, 10, std::hash<int>(), A());
  CHECK(sorted(l1) == sorted(r1) && sorted(l2) == sorted(r1) && sorted(l3) == sorted(r1));
  CHECK(sorted(l4) == sorted(r1) && sorted(l5) == sorted(r1));

  S cp(l1);
  CHECK(cp == l1);
  S cpa(l1, A());
  CHECK(cpa == l1);
  S mv(std::move(cp));
  CHECK(mv == l1);
  S mva(std::move(cpa), A());
  CHECK(mva == l1);

  Set<int, SeededHash> sh(10, SeededHash(12345));
  CHECK(sh.hash_function().seed == 12345);
  Set<int, ModHash, ModEqual> mod;
  mod.insert(5);
  CHECK(!mod.insert(1005).second);  // equal to 5 under ModEqual
  CHECK(mod.find(2005) != mod.end() && *mod.find(2005) == 5);
}

// ------------------------------------------------------------ assignment
template <template <typename...> class Set>
void test_assignment_and_swap() {
  using S = Set<std::string>;
  S a{"a", "b"};
  S b{"c"};
  b = a;
  CHECK(b == a && b.size() == 2);
  S& self = b;
  b = self;
  CHECK(b == a);
  S c;
  c = std::move(b);
  CHECK(c == a);
  c = {"x", "y", "z"};
  CHECK(c.size() == 3 && c.count("y") == 1);

  S x{"one"};
  S y{"two", "three"};
  x.swap(y);
  CHECK(x.size() == 2 && y.size() == 1 && y.count("one"));
  using std::swap;
  swap(x, y);
  CHECK(x.size() == 1 && x.count("one"));
  static_assert(noexcept(x.swap(y)), "swap of std::allocator containers is noexcept");
}

// ------------------------------------------------------------- iterators
template <template <typename...> class Set>
void test_iterators() {
  using S = Set<int>;
  S s;
  for (int i = 0; i < 1000; ++i) s.insert(i);
  std::vector<int> seen(s.begin(), s.end());
  std::sort(seen.begin(), seen.end());
  CHECK(seen.size() == 1000 && seen.front() == 0 && seen.back() == 999);
  CHECK(std::adjacent_find(seen.begin(), seen.end()) == seen.end());
  CHECK(static_cast<size_t>(std::distance(s.cbegin(), s.cend())) == s.size());
  const S& cs = s;
  CHECK(static_cast<size_t>(std::distance(cs.begin(), cs.end())) == s.size());

  auto it = s.begin();
  auto old = it++;
  CHECK(old != it && old == s.begin());
  typename S::const_iterator ci = s.begin();
  CHECK(ci == s.begin() && s.begin() == ci);

  auto keep = s.find(999);
  size_t visited = 0;
  for (auto e = s.begin(); e != s.end();) {
    ++visited;
    if (*e % 2 == 0) e = s.erase(e);
    else ++e;
  }
  CHECK(visited == 1000 && s.size() == 500 && *keep == 999);

  S empty;
  CHECK(empty.begin() == empty.end() && empty.cbegin() == empty.cend());
}

// ---------------------------------------------------------------- capacity
template <template <typename...> class Set>
void test_capacity() {
  Set<int> s;
  CHECK(s.empty() && s.size() == 0 && s.max_size() > 1000000);
  s.insert(1);
  CHECK(!s.empty() && s.size() == 1);
}

// --------------------------------------------------------------- modifiers
template <template <typename...> class Set>
void test_insert_emplace() {
  using S = Set<std::string>;
  S s;
  const std::string one = "one";
  auto r = s.insert(one);
  CHECK(r.second && *r.first == "one");
  r = s.insert(std::string("one"));
  CHECK(!r.second && *r.first == "one");
  std::string two = "two";
  r = s.insert(std::move(two));
  CHECK(r.second && *r.first == "two");
  auto it = s.insert(s.cbegin(), "three");
  CHECK(*it == "three");
  it = s.insert(s.cend(), std::string("three"));
  CHECK(*it == "three" && s.size() == 3);
  std::vector<std::string> more{"four", "five", "one"};
  s.insert(more.begin(), more.end());
  s.insert({"six", "seven"});
  CHECK(s.size() == 7);

  r = s.emplace("eight");
  CHECK(r.second && *r.first == "eight");
  r = s.emplace(3, 'x');  // constructor arguments
  CHECK(r.second && *r.first == "xxx");
  r = s.emplace("xxx");
  CHECK(!r.second);
  it = s.emplace_hint(s.cbegin(), "nine");
  CHECK(*it == "nine");
  it = s.emplace_hint(s.cbegin(), "nine");
  CHECK(*it == "nine" && s.size() == 10);

  // Move-only keys.
  Set<std::unique_ptr<int>> up;
  for (int i = 0; i < 300; ++i) up.emplace(std::make_unique<int>(i));  // growth
  CHECK(up.size() == 300);
  size_t sum = 0;
  for (const auto& p : up) sum += static_cast<size_t>(*p);
  CHECK(sum == 299 * 300 / 2);
  Set<std::unique_ptr<int>> up2(std::move(up));
  CHECK(up2.size() == 300);
}

template <template <typename...> class Set>
void test_erase_clear() {
  using S = Set<int>;
  S s;
  for (int i = 0; i < 100; ++i) s.insert(i);
  CHECK(s.erase(5) == 1 && s.erase(5) == 0 && s.size() == 99);
  auto it = s.find(10);
  auto next = std::next(it);
  CHECK(s.erase(it) == next && s.count(10) == 0);
  typename S::const_iterator cit = s.find(11);
  s.erase(cit);
  CHECK(s.count(11) == 0);
  auto first = s.begin();
  auto last = std::next(first, 10);
  std::vector<int> gone(first, last);
  CHECK(s.erase(first, last) == last);
  for (int k : gone) CHECK(s.count(k) == 0);
  CHECK(s.size() == 87);
  CHECK(s.erase(s.begin(), s.begin()) == s.begin());
  s.erase(s.begin(), s.end());
  CHECK(s.empty());
  for (int i = 0; i < 100; ++i) s.insert(i);
  s.clear();
  CHECK(s.empty() && s.begin() == s.end() && s.find(1) == s.end());
  s.insert(1);
  CHECK(s.size() == 1);
}

// ------------------------------------------------------------------ lookup
template <template <typename...> class Set>
void test_lookup() {
  using S = Set<std::string>;
  S s{"a", "b"};
  const S& cs = s;
  CHECK(s.count("a") == 1 && s.count("zz") == 0);
  CHECK(s.find("a") != s.end() && *s.find("a") == "a");
  CHECK(cs.find("b") != cs.end() && cs.find("zz") == cs.end());
  auto er = s.equal_range("b");
  CHECK(std::distance(er.first, er.second) == 1 && *er.first == "b");
  auto cer = cs.equal_range("zz");
  CHECK(cer.first == cer.second && cer.first == cs.end());
#if __cplusplus >= 202002L
  CHECK(s.contains("a") && !s.contains("zz"));
#endif
}

// ------------------------------------------------------------------ buckets
template <typename S>
void check_buckets(const S& s) {
  const size_t bc = s.bucket_count();
  CHECK(bc > 0 && s.max_bucket_count() >= bc);
  size_t total = 0;
  for (size_t n = 0; n < bc; ++n) {
    const size_t sz = s.bucket_size(n);
    CHECK(sz == static_cast<size_t>(std::distance(s.begin(n), s.end(n))));
    CHECK(static_cast<size_t>(std::distance(s.cbegin(n), s.cend(n))) == sz);
    for (auto lit = s.begin(n); lit != s.end(n); ++lit) CHECK(s.bucket(*lit) == n);
    total += sz;
  }
  CHECK(total == s.size());
  for (const auto& k : s) {
    const size_t n = s.bucket(k);
    CHECK(n < bc);
    bool found = false;
    for (auto lit = s.begin(n); lit != s.end(n); ++lit) found |= (*lit == k);
    CHECK(found);
  }
}

template <template <typename...> class Set>
void test_bucket_interface() {
  using S = Set<int>;
  S s;
  for (int i = 0; i < 2000; ++i) s.insert(i * 7919);
  check_buckets(s);  // sflat: just after a growth, the migration is running
  s.rehash(0);
  check_buckets(s);
  S g;
  size_t last = g.bucket_count();
  for (int i = 0; i < 20000; ++i) {
    g.insert(i);
    if (g.bucket_count() != last) {
      last = g.bucket_count();
      if (g.size() > 1000) check_buckets(g);
    }
  }
}

// -------------------------------------------------------------- hash policy
template <template <typename...> class Set>
void test_hash_policy() {
  using S = Set<int>;
  S s;
  CHECK(s.load_factor() == 0);
  for (int i = 0; i < 1000; ++i) s.insert(i);
  CHECK(s.load_factor() > 0 && s.load_factor() <= s.max_load_factor());
  CHECK(std::abs(s.load_factor() - static_cast<double>(s.size()) / s.bucket_count()) < 1e-6);
  // max_load_factor(z) is a hint (libc++ keeps it >= the current load
  // factor); the portable guarantee is load_factor() <= max_load_factor().
  s.max_load_factor(0.5f);
  CHECK(s.load_factor() <= s.max_load_factor() + 1e-6);
  for (int i = 0; i < 1000; ++i) CHECK(s.count(i) == 1);
  S low;
  low.max_load_factor(0.5f);
  CHECK(std::abs(low.max_load_factor() - 0.5) < 1e-6);
  for (int i = 0; i < 1000; ++i) low.insert(i);
  CHECK(low.load_factor() <= 0.5 + 1e-6);

  s.rehash(5000);
  CHECK(s.bucket_count() >= 5000 && s.size() == 1000);
  s.rehash(0);
  CHECK(s.load_factor() <= s.max_load_factor() + 1e-6);

  for (size_t n : {1u, 7u, 14u, 100u, 1000u, 12345u}) {
    S r;
    r.reserve(n);
    const size_t bc = r.bucket_count();
    CHECK(bc * r.max_load_factor() >= n - 1e-6);
    for (size_t i = 0; i < n; ++i) r.insert(static_cast<int>(i));
    CHECK(r.bucket_count() == bc);
  }
}

// ---------------------------------------------------------------- observers
template <template <typename...> class Set>
void test_observers() {
  Set<int, SeededHash> s(0, SeededHash(42));
  CHECK(s.hash_function().seed == 42 && s.hash_function()(5) == SeededHash(42)(5));
  Set<int, ModHash, ModEqual> e;
  CHECK(e.key_eq()(3, 1003) && !e.key_eq()(3, 4));
  Set<int> a;
  CHECK(a.get_allocator() == std::allocator<int>());
}

// ------------------------------------------------------------- non-members
template <template <typename...> class Set>
void test_comparison() {
  using S = Set<int>;
  S a{1, 2, 3};
  S b{3, 2, 1};
  CHECK(a == b && !(a != b));
  b.insert(4);
  CHECK(a != b && !(a == b));
  b.erase(4);
  b.erase(3);
  b.insert(5);
  CHECK(a != b);  // same size, different keys
  S e1, e2;
  CHECK(e1 == e2);
}

template <template <typename...> class Set>
void run_all(const char* name) {
  test_member_types<Set>();
  test_constructors<Set>();
  test_assignment_and_swap<Set>();
  test_iterators<Set>();
  test_capacity<Set>();
  test_insert_emplace<Set>();
  test_erase_clear<Set>();
  test_lookup<Set>();
  test_bucket_interface<Set>();
  test_hash_policy<Set>();
  test_observers<Set>();
  test_comparison<Set>();
  std::printf("%s: all interface tests OK\n", name);
}

template <typename K, typename H = std::hash<K>, typename E = std::equal_to<K>>
using StdSet = std::unordered_set<K, H, E>;
template <typename K, typename H = std::hash<K>, typename E = std::equal_to<K>>
using SflatSet = sflat::flat_hash_set<K, H, E>;

int main() {
  run_all<StdSet>("std::unordered_set");
  run_all<SflatSet>("sflat::flat_hash_set");
  std::printf("ALL SET INTERFACE TESTS PASSED\n");
  return 0;
}
