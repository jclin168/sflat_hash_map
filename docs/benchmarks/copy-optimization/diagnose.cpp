#include <sflat/flat_hash_map.hpp>
#include "sfc64.h"
#include <cstdint>
#include <cstdio>
#include <functional>
struct Hash {
  static uint64_t calls;
  size_t operator()(uint64_t key) const noexcept { ++calls; return std::hash<uint64_t>{}(key); }
};
uint64_t Hash::calls=0;
int main() {
  using M=sflat::flat_hash_map<uint64_t,uint64_t,Hash>;
  sfc64 rng(987); M source;
  for(uint64_t i=0;i<1000000;++i) source[rng()]=i;
  Hash::calls=0; M copying=source;
  std::printf("source_capacity=%zu copy_capacity=%zu initial_copy_hashes=%llu max_load=%.3f\n",source.bucket_count(),copying.bucket_count(),(unsigned long long)Hash::calls,copying.max_load_factor());
  uint64_t previous=~uint64_t(0),total=0;size_t cap=0;
  for(unsigned i=0;i<200;++i) {
    Hash::calls=0; M m=copying;
    uint64_t calls=Hash::calls; total+=calls;
    if(calls!=previous||copying.bucket_count()!=cap||i==199) std::printf("iteration=%u entries=%zu capacity=%zu copy_capacity=%zu copy_hashes=%llu\n",i,copying.size(),copying.bucket_count(),m.bucket_count(),(unsigned long long)calls);
    previous=calls;cap=copying.bucket_count();
    copying[rng()]=rng();
  }
  std::printf("total_copy_hashes=%llu\n",(unsigned long long)total);
}
