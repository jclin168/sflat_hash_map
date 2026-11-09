// SPDX-License-Identifier: Apache-2.0
// Compiled to assembly by tools/check_branchless.py with
// -DSFLAT_CHECK_BRANCHLESS: the code between the sflat-branchless markers
// must have no branch instruction. Not a program to run.
#include <sflat/flat_hash_map.hpp>
#include <sflat/flat_hash_set.hpp>

#include <cstddef>
#include <cstdint>
#include <string>

using Map = sflat::flat_hash_map<uint64_t, uint64_t>;
using Set = sflat::flat_hash_set<uint64_t>;
using StrMap = sflat::flat_hash_map<std::string, uint64_t>;

// Erase by key (the home-group path and the general path).
size_t erase_keys(Map& m, const uint64_t* k, size_t n) {
  size_t r = 0;
  for (size_t i = 0; i < n; ++i) r += m.erase(k[i]);
  return r;
}
size_t erase_keys(Set& s, const uint64_t* k, size_t n) {
  size_t r = 0;
  for (size_t i = 0; i < n; ++i) r += s.erase(k[i]);
  return r;
}
size_t erase_keys(StrMap& m, const std::string* k, size_t n) {
  size_t r = 0;
  for (size_t i = 0; i < n; ++i) r += m.erase(k[i]);
  return r;
}
// Erase by iterator (mark_erased()).
void erase_iter(Map& m) { m.erase(m.begin()); }
void erase_iter(Set& s) { s.erase(s.begin()); }
