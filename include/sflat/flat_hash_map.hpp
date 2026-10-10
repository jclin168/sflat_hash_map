#pragma once
//
// sflat::flat_hash_map
// =====================
// An open-addressing hash map with an std::unordered_map-compatible interface
// (C++17), designed for very large tables:
//
//  * SIMD group probing (SSE2 x16, AVX2 x32 when available) à la SwissTable.
//  * The growth factor shrinks as the table gets large (2x, then 1.5x, then
//    1.25x; see SFLAT_GROW_2X_BELOW). Capacities are any multiple of the
//    group width (not restricted to powers of two) thanks to Lemire's
//    "fastrange" mapping. This bounds memory waste for very large tables.
//  * Incremental growth: the old table is migrated in small steps, and (on
//    Linux, with std::allocator) the memory behind the migration front is
//    given back to the kernel, so growth does not need old + new at once.
//  * Large arrays use transparent huge pages (THP) on Linux to cut TLB misses.
//  * Hot paths are insert() and find(); erase() uses tombstones and
//    iteration is a plain linear scan.
//  * The user hash is passed through a single-multiply mixer so the map
//    stays robust even with weak hashes (e.g. std::hash<uint64_t> identity on
//    sequential keys).
//
// Layout: one control byte per slot (SwissTable-style: H2 in [0,0xFD],
// 0xFF = empty, 0xFE = deleted) stored in a dense array, a separate slot array
// (std::pair<const Key, T> in aligned raw storage), and one 16-bit overflow
// word per group.

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <utility>

#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <emmintrin.h>  // SSE2 (always present on x86-64)
#if defined(__AVX2__)
#include <immintrin.h>
#endif
#endif

// SFLAT_USE_MMAP=0 disables the mmap memory path (default: on for Linux).
// SFLAT_USE_THP=0 keeps mmap and page release but does not ask for
// transparent huge pages (default: on).
#if !defined(SFLAT_USE_THP)
#define SFLAT_USE_THP 1
#endif
// Growth tiers, by the size of the current table in bytes (slots plus control
// bytes): below SFLAT_GROW_2X_BELOW the table grows 2x, below
// SFLAT_GROW_1_5X_BELOW it grows 1.5x, else 1.25x.
#if !defined(SFLAT_GROW_2X_BELOW)
#define SFLAT_GROW_2X_BELOW (2ULL << 30)  // 2 GiB
#endif
#if !defined(SFLAT_GROW_1_5X_BELOW)
#define SFLAT_GROW_1_5X_BELOW (10ULL << 30)  // 10 GiB
#endif
#if !defined(SFLAT_USE_MMAP)
#if defined(__linux__)
#define SFLAT_USE_MMAP 1
#else
#define SFLAT_USE_MMAP 0
#endif
#endif

#if SFLAT_USE_MMAP
#include <sys/mman.h>
#define SFLAT_HAS_MMAP 1
#ifndef MADV_COLLAPSE
#define MADV_COLLAPSE 25
#endif
#else
#define SFLAT_HAS_MMAP 0
#endif

namespace sflat {
namespace detail {

inline constexpr uint8_t kEmpty = 0xFF;    // never used as H2
inline constexpr uint8_t kDeleted = 0xFE;  // tombstone
inline constexpr bool IsFull(uint8_t c) noexcept { return c < 0xFE; }

// Low 8 bits of the (mixed) hash, clamped to [0,253] to avoid colliding with
// kEmpty/kDeleted (one compare and one cmov). Gives ~2x fewer spurious H2
// matches than a 7-bit fingerprint.
inline uint8_t H2(uint64_t h) noexcept {
  const uint8_t x = static_cast<uint8_t>(h);
  return x < 0xFD ? x : static_cast<uint8_t>(0xFD);
}

// Control bytes of an empty table (capacity 0): one all-empty group, so a
// find needs no capacity check.
struct EmptyCtrl {
  alignas(64) uint8_t b[64];
  constexpr EmptyCtrl() : b{} {
    for (auto& x : b) x = kEmpty;
  }
};
inline constexpr EmptyCtrl kEmptyCtrl{};

// Single-multiply avalanche mixer, boost-style mulx64:
//   (x * C) ^ ((x * C) >> 64),  C = 2^64/phi (golden ratio).
// One 128-bit multiply instead of a splitmix64 finalizer (which needs two
// *dependent* 64-bit multiplies): much shorter critical path, while still
// robust against weak hashes such as std::hash<uint64_t> identity on
// sequential keys.
inline uint64_t MixHash(uint64_t x) noexcept {
  const __uint128_t r = (__uint128_t)x * 0x9E3779B97F4A7C15ULL;
  return (uint64_t)r ^ (uint64_t)(r >> 64);
}

// 32-bit Lemire fastrange: maps h uniformly into [0, n) with a single
// 32x32->64 IMUL (1 uop, no implicit registers). A 128-bit multiply here
// decodes to several uops and serializes the probe loop, costing ~5x.
// Unlike a bitmask, n does not need to be a power of two, which is what lets
// us grow by 1.5x without rounding up to the next power of two (2x).
// Requires n < 2^32 (4G groups = 137G slots — far beyond practical tables).
// Uses bits [32,64) of the mixed hash, disjoint from H2's bits [0,8).
//
// The mapping is monotonic in h: a key's home group in a table of n groups
// and in a table of m groups are at the same relative position. Growth
// relies on this: migrating the old table in index order writes the new
// table in (almost) index order too.
inline size_t Fastrange(uint64_t h, size_t n) noexcept {
  return (size_t)(((uint64_t)(uint32_t)n * (uint32_t)(h >> 32)) >> 32);
}

// Per-group overflow Bloom bit: when a key's home group is full and the key
// spills to a later group, set this bit on the home group. Find can then
// terminate a miss after the home group if the bit is clear.
// 16 bits per group: a 32-slot group at 0.875 load often spills, and 8 bits
// fill up fast (a miss then probes the next group).
// Uses hash bits [8,12), disjoint from H2 bits [0,8) and position bits [32,64).
using OverflowWord = uint16_t;
inline OverflowWord OverflowBit(uint64_t h) noexcept {
  return static_cast<OverflowWord>(1u << ((h >> 8) & 15u));
}

inline unsigned Ctzb(uint32_t x) noexcept {
#if defined(_MSC_VER)
  unsigned long i;
  _BitScanForward(&i, x);
  return static_cast<unsigned>(i);
#else
  return static_cast<unsigned>(__builtin_ctz(x));
#endif
}

#if defined(__AVX2__)
// 32 control bytes per group.
struct Group {
  static constexpr size_t kWidth = 32;
  __m256i v;
  static Group Load(const uint8_t* p) noexcept {
    Group g;
    g.v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(p));
    return g;
  }
  // Bitmask of lanes equal to c.
  uint32_t Match(uint8_t c) const noexcept {
    return static_cast<uint32_t>(_mm256_movemask_epi8(
        _mm256_cmpeq_epi8(v, _mm256_set1_epi8(static_cast<char>(c)))));
  }
};
#else
// 16 control bytes per group (SSE2 baseline, always available on x86-64).
struct Group {
  static constexpr size_t kWidth = 16;
  __m128i v;
  static Group Load(const uint8_t* p) noexcept {
    Group g;
    g.v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p));
    return g;
  }
  uint32_t Match(uint8_t c) const noexcept {
    return static_cast<uint32_t>(_mm_movemask_epi8(
               _mm_cmpeq_epi8(v, _mm_set1_epi8(static_cast<char>(c))))) &
           0xFFFFu;
  }
};
#endif

// ---------------------------------------------------------------- page memory
// With std::allocator the map manages its own memory:
//  * arrays of 2 MiB or more get their own 2 MiB-aligned anonymous mapping, so
//    the map can ask for transparent huge pages (THP) and give pages back to
//    the kernel during growth;
//  * smaller arrays come from operator new with cache-line alignment, so a
//    control group never straddles two cache lines.
inline constexpr size_t kHugePage = size_t(2) << 20;
inline constexpr size_t kCacheLine = 64;

inline constexpr size_t RoundUp(size_t x, size_t a) noexcept {
  return (x + a - 1) / a * a;
}

// True if an array of this many bytes gets its own mapping.
inline constexpr bool IsMapped(size_t bytes) noexcept {
  return SFLAT_HAS_MMAP && bytes >= kHugePage;
}

// huge: MADV_HUGEPAGE for the whole array (else MADV_NOHUGEPAGE).
inline void* PageAlloc(size_t bytes, size_t align, bool huge) {
#if SFLAT_HAS_MMAP
  if (IsMapped(bytes)) {
    const size_t len = RoundUp(bytes, kHugePage);
    void* raw = ::mmap(nullptr, len + kHugePage, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (raw == MAP_FAILED) throw std::bad_alloc();
    const uintptr_t r = reinterpret_cast<uintptr_t>(raw);
    const uintptr_t a = RoundUp(r, kHugePage);
    if (a != r) ::munmap(raw, a - r);
    const uintptr_t tail = (r + len + kHugePage) - (a + len);
    if (tail) ::munmap(reinterpret_cast<void*>(a + len), tail);
    void* p = reinterpret_cast<void*>(a);
    if (SFLAT_USE_THP) ::madvise(p, len, huge ? MADV_HUGEPAGE : MADV_NOHUGEPAGE);
    return p;
  }
#endif
  (void)huge;
  return ::operator new(bytes, std::align_val_t(align));
}

inline void PageFree(void* p, size_t bytes, size_t align) noexcept {
  if (!p) return;
#if SFLAT_HAS_MMAP
  if (IsMapped(bytes)) {
    ::munmap(p, RoundUp(bytes, kHugePage));
    return;
  }
#endif
  ::operator delete(p, std::align_val_t(align));
}

// Give the whole huge pages inside [from, to) of a mapped array of `total`
// bytes back to the kernel. Their contents must not be read again.
inline void PageRelease(void* p, size_t total, size_t from, size_t to) noexcept {
#if SFLAT_HAS_MMAP
  if (!IsMapped(total)) return;
  from = RoundUp(from, kHugePage);
  to = to / kHugePage * kHugePage;
  if (to > from)
    ::madvise(static_cast<char*>(p) + from, to - from, MADV_DONTNEED);
#else
  (void)p, (void)total, (void)from, (void)to;
#endif
}

// Ask for huge pages in [from, to) of a mapped array of `total` bytes (rounded
// out to whole huge pages). With collapse, also convert pages that are already
// present (MADV_COLLAPSE, Linux 6.1+; silently ignored on older kernels).
inline void PageHuge(void* p, size_t total, size_t from, size_t to,
                     bool collapse) noexcept {
#if SFLAT_HAS_MMAP
  if (!SFLAT_USE_THP || !IsMapped(total)) return;
  const size_t len = RoundUp(total, kHugePage);
  from = from / kHugePage * kHugePage;
  to = RoundUp(to, kHugePage);
  if (to > len) to = len;
  if (to <= from) return;
  char* b = static_cast<char*>(p) + from;
  ::madvise(b, to - from, MADV_HUGEPAGE);
  if (collapse) ::madvise(b, to - from, MADV_COLLAPSE);
#else
  (void)p, (void)total, (void)from, (void)to, (void)collapse;
#endif
}

}  // namespace detail

template <typename Key, typename T, typename Hash = std::hash<Key>,
          typename KeyEqual = std::equal_to<Key>,
          typename Allocator = std::allocator<std::pair<const Key, T>>>
class flat_hash_map {
 public:
  using key_type = Key;
  using mapped_type = T;
  using value_type = std::pair<const Key, T>;
  using size_type = std::size_t;
  using difference_type = std::ptrdiff_t;
  using hasher = Hash;
  using key_equal = KeyEqual;
  using allocator_type = Allocator;
  using reference = value_type&;
  using const_reference = const value_type&;
  using pointer = typename std::allocator_traits<Allocator>::pointer;
  using const_pointer = typename std::allocator_traits<Allocator>::const_pointer;

 private:
  using slot_type = typename std::aligned_storage<sizeof(value_type),
                                                  alignof(value_type)>::type;
  using slot_alloc = typename std::allocator_traits<Allocator>::template rebind_alloc<slot_type>;
  using slot_traits = std::allocator_traits<slot_alloc>;
  using ctrl_alloc = typename std::allocator_traits<Allocator>::template rebind_alloc<uint8_t>;
  using ctrl_traits = std::allocator_traits<ctrl_alloc>;

  static constexpr size_t kWidth = detail::Group::kWidth;
  static constexpr double kDefaultMaxLoadFactor = 0.875;
  // Largest capacity: Fastrange is 32-bit, so fewer than 2^32 groups (with
  // AVX2 about 137G slots), and the slot array must fit in size_t.
  static constexpr size_t kMaxCapacity =
      static_cast<size_t>(
          ((uint64_t{1} << 32) - 1) * kWidth <
                  std::numeric_limits<size_t>::max() / sizeof(slot_type)
              ? ((uint64_t{1} << 32) - 1) * kWidth
              : std::numeric_limits<size_t>::max() / sizeof(slot_type)) /
      kWidth * kWidth;
  // Old slots migrated per insert during growth.
  static constexpr size_t kMigrateBatch = 64;  // multiple of kWidth
  // With std::allocator the map uses detail::PageAlloc (THP, page release).
  static constexpr bool kOwnMemory =
      std::is_same<Allocator, std::allocator<value_type>>::value;
  static constexpr size_t kSlotAlign =
      alignof(slot_type) > detail::kCacheLine ? alignof(slot_type)
                                              : detail::kCacheLine;

 public:
  // ---------------------------------------------------------------- iterators
  // An iterator holds a pointer to its element (nullptr at the end). The
  // unified slot index is [0, cap_) for the current table and
  // [cap_, cap_ + old_cap_) for the old table of an ongoing migration.
  template <bool IsConst>
  class iterator_impl {
   public:
    using iterator_category = std::forward_iterator_tag;
    using value_type = typename flat_hash_map::value_type;
    using difference_type = std::ptrdiff_t;
    using reference =
        typename std::conditional<IsConst, const value_type&, value_type&>::type;
    using pointer =
        typename std::conditional<IsConst, const value_type*, value_type*>::type;

    iterator_impl() noexcept = default;

    // iterator -> const_iterator conversion.
    template <bool O, typename = typename std::enable_if<IsConst && !O>::type>
    iterator_impl(const iterator_impl<O>& o) noexcept
        : map_(o.map_), p_(o.p_) {}

    reference operator*() const noexcept { return *p_; }
    pointer operator->() const noexcept { return p_; }

    iterator_impl& operator++() noexcept {
      p_ = map_->first_full_from(map_->index_of(p_) + 1);
      return *this;
    }
    iterator_impl operator++(int) noexcept {
      iterator_impl c(*this);
      ++*this;
      return c;
    }

    // Unified slot index (cap_ + old_cap_ at the end).
    size_t index() const noexcept { return map_->index_of(p_); }

    template <bool O>
    bool operator==(const iterator_impl<O>& o) const noexcept {
      return p_ == o.p_;
    }
    template <bool O>
    bool operator!=(const iterator_impl<O>& o) const noexcept {
      return p_ != o.p_;
    }

   private:
    friend class flat_hash_map;
    template <bool>
    friend class iterator_impl;
    using map_ptr = typename std::conditional<IsConst, const flat_hash_map*,
                                              flat_hash_map*>::type;

    iterator_impl(map_ptr m, pointer p) noexcept : map_(m), p_(p) {}

    map_ptr map_ = nullptr;
    pointer p_ = nullptr;
  };

  using iterator = iterator_impl<false>;
  using const_iterator = iterator_impl<true>;

  // Local iterators scan the whole table and filter by bucket index.
  template <bool IsConst>
  class local_iterator_impl {
   public:
    using iterator_category = std::forward_iterator_tag;
    using value_type = typename flat_hash_map::value_type;
    using difference_type = std::ptrdiff_t;
    using reference =
        typename std::conditional<IsConst, const value_type&, value_type&>::type;
    using pointer =
        typename std::conditional<IsConst, const value_type*, value_type*>::type;

    local_iterator_impl() noexcept = default;

    template <bool O, typename = typename std::enable_if<IsConst && !O>::type>
    local_iterator_impl(const local_iterator_impl<O>& o) noexcept
        : map_(o.map_), bucket_(o.bucket_), idx_(o.idx_) {}

    reference operator*() const noexcept { return *map_->slot_at(idx_); }
    pointer operator->() const noexcept { return map_->slot_at(idx_); }

    local_iterator_impl& operator++() noexcept {
      ++idx_;
      seek();
      return *this;
    }
    local_iterator_impl operator++(int) noexcept {
      local_iterator_impl c(*this);
      ++*this;
      return c;
    }

    template <bool O>
    bool operator==(const local_iterator_impl<O>& o) const noexcept {
      return map_ == o.map_ && bucket_ == o.bucket_ && idx_ == o.idx_;
    }
    template <bool O>
    bool operator!=(const local_iterator_impl<O>& o) const noexcept {
      return !(*this == o);
    }

   private:
    friend class flat_hash_map;
    template <bool>
    friend class local_iterator_impl;
    using map_ptr = typename std::conditional<IsConst, const flat_hash_map*,
                                              flat_hash_map*>::type;

    local_iterator_impl(map_ptr m, size_t b, size_t i) noexcept
        : map_(m), bucket_(b), idx_(i) {
      if (map_) seek();
    }
    void seek() noexcept {
      while (idx_ < map_->cap_ &&
             !(detail::IsFull(map_->ctrl_[idx_]) &&
               map_->bucket_index(map_->slot_at(idx_)->first) == bucket_))
        ++idx_;
    }

    map_ptr map_ = nullptr;
    size_t bucket_ = 0;
    size_t idx_ = 0;
  };

  using local_iterator = local_iterator_impl<false>;
  using const_local_iterator = local_iterator_impl<true>;

  // ------------------------------------------------------------ construction
 public:
  flat_hash_map() noexcept(
      std::is_nothrow_default_constructible<Hash>::value &&
      std::is_nothrow_default_constructible<KeyEqual>::value &&
      std::is_nothrow_default_constructible<Allocator>::value) {}

  explicit flat_hash_map(size_type bucket_count, const Hash& hash = Hash(),
                         const KeyEqual& equal = KeyEqual(),
                         const Allocator& alloc = Allocator())
      : hash_(hash), equal_(equal), slot_alloc_(alloc), ctrl_alloc_(alloc) {
    if (bucket_count) rehash_to(min_cap_for(bucket_count));
  }

  flat_hash_map(size_type bucket_count, const Allocator& alloc)
      : flat_hash_map(bucket_count, Hash(), KeyEqual(), alloc) {}
  flat_hash_map(size_type bucket_count, const Hash& hash, const Allocator& alloc)
      : flat_hash_map(bucket_count, hash, KeyEqual(), alloc) {}

  explicit flat_hash_map(const Allocator& alloc)
      : hash_(), equal_(), slot_alloc_(alloc), ctrl_alloc_(alloc) {}

  template <typename InputIt>
  flat_hash_map(InputIt first, InputIt last,
                size_type bucket_count = 0, const Hash& hash = Hash(),
                const KeyEqual& equal = KeyEqual(),
                const Allocator& alloc = Allocator())
      : flat_hash_map(bucket_count, hash, equal, alloc) {
    insert(first, last);
  }

  template <typename InputIt>
  flat_hash_map(InputIt first, InputIt last, size_type bucket_count,
                const Allocator& alloc)
      : flat_hash_map(first, last, bucket_count, Hash(), KeyEqual(), alloc) {}

  template <typename InputIt>
  flat_hash_map(InputIt first, InputIt last, size_type bucket_count,
                const Hash& hash, const Allocator& alloc)
      : flat_hash_map(first, last, bucket_count, hash, KeyEqual(), alloc) {}

  flat_hash_map(std::initializer_list<value_type> il,
                size_type bucket_count = 0, const Hash& hash = Hash(),
                const KeyEqual& equal = KeyEqual(),
                const Allocator& alloc = Allocator())
      : flat_hash_map(il.begin(), il.end(), bucket_count, hash, equal, alloc) {}

  flat_hash_map(std::initializer_list<value_type> il, size_type bucket_count,
                const Allocator& alloc)
      : flat_hash_map(il, bucket_count, Hash(), KeyEqual(), alloc) {}

  flat_hash_map(std::initializer_list<value_type> il, size_type bucket_count,
                const Hash& hash, const Allocator& alloc)
      : flat_hash_map(il, bucket_count, hash, KeyEqual(), alloc) {}

  flat_hash_map(const flat_hash_map& other)
      : max_load_factor_(other.max_load_factor_),
        hash_(other.hash_),
        equal_(other.equal_),
        slot_alloc_(slot_traits::select_on_container_copy_construction(
            other.slot_alloc_)),
        ctrl_alloc_(ctrl_traits::select_on_container_copy_construction(
            other.ctrl_alloc_)) {
    copy_elements_from(other);
  }

  flat_hash_map(const flat_hash_map& other, const Allocator& alloc)
      : max_load_factor_(other.max_load_factor_),
        hash_(other.hash_),
        equal_(other.equal_),
        slot_alloc_(alloc),
        ctrl_alloc_(alloc) {
    copy_elements_from(other);
  }

  flat_hash_map(flat_hash_map&& other) noexcept
      : max_load_factor_(other.max_load_factor_),
        hash_(std::move(other.hash_)),
        equal_(std::move(other.equal_)),
        slot_alloc_(std::move(other.slot_alloc_)),
        ctrl_alloc_(std::move(other.ctrl_alloc_)) {
    steal_storage(other);
  }

  flat_hash_map(flat_hash_map&& other, const Allocator& alloc)
      : max_load_factor_(other.max_load_factor_),
        hash_(std::move(other.hash_)),
        equal_(std::move(other.equal_)),
        slot_alloc_(alloc),
        ctrl_alloc_(alloc) {
    if (alloc == other.get_allocator()) {
      steal_storage(other);
    } else {
      move_elements_from(other);
    }
  }

  ~flat_hash_map() {
    clear();
    free_storage();
  }

  flat_hash_map& operator=(const flat_hash_map& other) {
    if (this != &other) {
      flat_hash_map tmp(other);
      swap(tmp);
    }
    return *this;
  }

  flat_hash_map& operator=(flat_hash_map&& other) noexcept(
      slot_traits::propagate_on_container_move_assignment::value &&
      ctrl_traits::propagate_on_container_move_assignment::value) {
    if (this != &other) {
      clear();
      free_storage();
      if (slot_traits::propagate_on_container_move_assignment::value) {
        slot_alloc_ = std::move(other.slot_alloc_);
        ctrl_alloc_ = std::move(other.ctrl_alloc_);
        steal_storage(other);
      } else {
        // Allocators don't propagate: move elements one by one.
        move_elements_from(other);
      }
      hash_ = std::move(other.hash_);
      equal_ = std::move(other.equal_);
      max_load_factor_ = other.max_load_factor_;
      update_limit();
    }
    return *this;
  }

  flat_hash_map& operator=(std::initializer_list<value_type> il) {
    clear();
    insert(il);
    return *this;
  }

  void swap(flat_hash_map& other) noexcept(
      slot_traits::propagate_on_container_swap::value ||
      slot_traits::is_always_equal::value) {
    using std::swap;
    swap(slots_, other.slots_);
    swap(ctrl_, other.ctrl_);
    swap(overflow_, other.overflow_);
    swap(cap_, other.cap_);
    swap(size_, other.size_);
    swap(deleted_, other.deleted_);
    swap(limit_, other.limit_);
    swap(growth_left_, other.growth_left_);
    swap(max_load_factor_, other.max_load_factor_);
    swap(hash_, other.hash_);
    swap(equal_, other.equal_);
    swap(old_slots_, other.old_slots_);
    swap(old_ctrl_, other.old_ctrl_);
    swap(old_overflow_, other.old_overflow_);
    swap(old_cap_, other.old_cap_);
    swap(mig_pos_, other.mig_pos_);
    swap(old_released_, other.old_released_);
    swap(huge_hint_, other.huge_hint_);
    if (slot_traits::propagate_on_container_swap::value) {
      swap(slot_alloc_, other.slot_alloc_);
      swap(ctrl_alloc_, other.ctrl_alloc_);
    }
  }

  // --------------------------------------------------------------- iteration
  // Iteration covers both tables during a migration; it does not move items.
  iterator begin() noexcept { return iterator(this, first_full_from(0)); }
  const_iterator begin() const noexcept {
    return const_iterator(this, first_full_from(0));
  }
  const_iterator cbegin() const noexcept { return begin(); }
  iterator end() noexcept { return iterator(this, nullptr); }
  const_iterator end() const noexcept { return const_iterator(this, nullptr); }
  const_iterator cend() const noexcept { return end(); }

  bool empty() const noexcept { return size_ == 0; }
  size_type size() const noexcept { return size_; }
  size_type max_size() const noexcept {
    return static_cast<size_type>(static_cast<double>(kMaxCapacity) *
                                  max_load_factor_);
  }

  // Complete an ongoing growth migration. Inserts do this in small steps;
  // call it to free the old table at once.
  void finish_migration() {
    while (is_migrating()) migrate_step(old_cap_);
  }

  // ------------------------------------------------------------- core probes
 private:
  // Visit every live element: the current table, then the part of the old
  // table that is not migrated yet.
  template <typename Self, typename F>
  static void for_each_slot(Self& self, F&& f) {
    for (size_t i = 0; i < self.cap_ + self.old_cap_; ++i)
      if (detail::IsFull(self.ctrl_at(i))) f(self.slot_at(i));
  }

  // Place a value whose key is known to be absent (no duplicate check).
  // Precondition: not migrating.
  template <typename V>
  void place_unique(V&& v) {
    const uint64_t h = hash_mixed(v.first);
    const size_t dst = find_empty(h);
    construct_slot(dst, std::forward<V>(v));
    ctrl_[dst] = detail::H2(h);
  }

  // Precondition: *this is empty and has no storage.
  void copy_elements_from(const flat_hash_map& other) {
    if (other.size_ == 0) return;
    rehash_to(min_cap_for(other.size_));
    for_each_slot(other, [this](const value_type* s) { place_unique(*s); });
    size_ = other.size_;
    reset_growth_left();
  }

  // Precondition: *this is empty and has no storage.
  void move_elements_from(flat_hash_map& other) {
    if (other.size_ == 0) return;
    rehash_to(min_cap_for(other.size_));
    for_each_slot(other,
                  [this](value_type* s) { place_unique(std::move(*s)); });
    size_ = other.size_;
    reset_growth_left();
    other.clear();
  }

  // Take all storage of other (including an ongoing migration).
  // Precondition: *this has no storage.
  void steal_storage(flat_hash_map& other) noexcept {
    slots_ = other.slots_;
    ctrl_ = other.ctrl_;
    overflow_ = other.overflow_;
    cap_ = other.cap_;
    size_ = other.size_;
    deleted_ = other.deleted_;
    limit_ = other.limit_;
    growth_left_ = other.growth_left_;
    old_slots_ = other.old_slots_;
    old_ctrl_ = other.old_ctrl_;
    old_overflow_ = other.old_overflow_;
    old_cap_ = other.old_cap_;
    mig_pos_ = other.mig_pos_;
    old_released_ = other.old_released_;
    huge_hint_ = other.huge_hint_;
    other.slots_ = nullptr;
    other.ctrl_ = empty_ctrl();
    other.overflow_ = nullptr;
    other.cap_ = 0;
    other.size_ = 0;
    other.deleted_ = 0;
    other.limit_ = 0;
    other.growth_left_ = 0;
    other.old_slots_ = nullptr;
    other.old_ctrl_ = nullptr;
    other.old_overflow_ = nullptr;
    other.old_cap_ = 0;
    other.mig_pos_ = 0;
    other.old_released_ = 0;
    other.huge_hint_ = 0;
  }

  // Slot i of the current table.
  value_type* cur_slot(size_t i) noexcept {
    return reinterpret_cast<value_type*>(static_cast<void*>(&slots_[i]));
  }

  // Slot and control byte by unified index (see iterator_impl).
  value_type* slot_at(size_t i) noexcept {
    slot_type* s = i < cap_ ? &slots_[i] : &old_slots_[i - cap_];
    return reinterpret_cast<value_type*>(static_cast<void*>(s));
  }
  const value_type* slot_at(size_t i) const noexcept {
    const slot_type* s = i < cap_ ? &slots_[i] : &old_slots_[i - cap_];
    return reinterpret_cast<const value_type*>(static_cast<const void*>(s));
  }
  uint8_t ctrl_at(size_t i) const noexcept {
    return i < cap_ ? ctrl_[i] : old_ctrl_[i - cap_];
  }

  // Unified index of an element pointer (nullptr: the end index).
  size_t index_of(const value_type* p) const noexcept {
    if (!p) return cap_ + old_cap_;
    const uintptr_t a = reinterpret_cast<uintptr_t>(p);
    const uintptr_t b = reinterpret_cast<uintptr_t>(slots_);
    if (a - b < cap_ * sizeof(slot_type)) return (a - b) / sizeof(slot_type);
    return cap_ + (a - reinterpret_cast<uintptr_t>(old_slots_)) /
                      sizeof(slot_type);
  }

  // First full slot at unified index i or later, or nullptr.
  value_type* first_full_from(size_t i) const noexcept {
    for (; i < cap_; ++i)
      if (detail::IsFull(ctrl_[i])) return const_cast<value_type*>(slot_at(i));
    for (; i < cap_ + old_cap_; ++i)
      if (detail::IsFull(old_ctrl_[i - cap_]))
        return const_cast<value_type*>(slot_at(i));
    return nullptr;
  }

  static uint8_t* empty_ctrl() noexcept {
    return const_cast<uint8_t*>(detail::kEmptyCtrl.b);
  }

  template <typename... Args>
  value_type* construct_slot(size_t i, Args&&... args) {
    return ::new (static_cast<void*>(slot_at(i)))
        value_type(std::forward<Args>(args)...);
  }
  void destroy_slot(size_t i) noexcept { slot_at(i)->~value_type(); }

  uint64_t hash_mixed(const key_type& k) const {
    return detail::MixHash(static_cast<uint64_t>(hash_(k)));
  }

  // Element with key in the given table, or nullptr. A table with cap 0 must
  // have ctrl == empty_ctrl().
  static const value_type* find_in(const key_type& key, uint64_t h,
                                   const uint8_t* ctrl, const slot_type* slots,
                                   size_t cap, const detail::OverflowWord* overflow,
                                   const KeyEqual& equal) noexcept {
    const uint8_t hh = detail::H2(h);
    const size_t ng = cap / kWidth;
    const size_t home = detail::Fastrange(h, ng);
    {
      detail::Group g = detail::Group::Load(ctrl + home * kWidth);
      uint32_t m = g.Match(hh);
      while (m) {
        const unsigned j = detail::Ctzb(m);
        m &= m - 1;
        const value_type* e = reinterpret_cast<const value_type*>(
            &slots[home * kWidth + j]);
        if (equal(e->first, key)) return e;
      }
      if (g.Match(detail::kEmpty)) return nullptr;
      if (!(overflow[home] & detail::OverflowBit(h))) return nullptr;
    }
    size_t gi = home + 1;
    for (size_t k = 1; k < ng; ++k) {
      if (gi >= ng) gi -= ng;
      detail::Group g = detail::Group::Load(ctrl + gi * kWidth);
      uint32_t m = g.Match(hh);
      while (m) {
        const unsigned j = detail::Ctzb(m);
        m &= m - 1;
        const value_type* e = reinterpret_cast<const value_type*>(
            &slots[gi * kWidth + j]);
        if (equal(e->first, key)) return e;
      }
      if (g.Match(detail::kEmpty)) return nullptr;
      ++gi;
    }
    return nullptr;
  }

  // Element with key in the old table of an ongoing migration, or nullptr.
  // Groups behind the front are migrated: they hold no elements and no empty
  // slots (all deleted), so a plain probe would walk through all of them.
  // Instead:
  //  * home group behind the front with a clear overflow bit: the key never
  //    spilled, so it was migrated (new keys with such a home go to the new
  //    table);
  //  * else skip the migrated groups and continue at the front. Erase and
  //    migration never create empty slots, so a key that spilled is before
  //    the first group that has an empty slot.
  const value_type* find_in_old(const key_type& key, uint64_t h) const noexcept {
    const uint8_t hh = detail::H2(h);
    const size_t ng = old_cap_ / kWidth;
    const size_t front = mig_pos_ / kWidth;  // first group not migrated
    const size_t home = detail::Fastrange(h, ng);
    if (home < front && !(old_overflow_[home] & detail::OverflowBit(h)))
      return nullptr;
    size_t gi = home;
    for (size_t k = 0; k < ng; ++k, ++gi) {
      if (gi >= ng) gi = 0;
      if (gi < front) gi = front;
      if (gi >= ng) return nullptr;
      const detail::Group g = detail::Group::Load(old_ctrl_ + gi * kWidth);
      uint32_t m = g.Match(hh);
      while (m) {
        const value_type* e = reinterpret_cast<const value_type*>(
            &old_slots_[gi * kWidth + detail::Ctzb(m)]);
        if (equal_(e->first, key)) return e;
        m &= m - 1;
      }
      if (g.Match(detail::kEmpty)) return nullptr;
      if (gi == home && !(old_overflow_[home] & detail::OverflowBit(h)))
        return nullptr;
    }
    return nullptr;
  }

  // Element with key, or nullptr. Never modifies the map.
  value_type* find_ptr(const key_type& key, uint64_t h) const noexcept {
    const value_type* p =
        find_in(key, h, ctrl_, slots_, cap_, overflow_, equal_);
    if (__builtin_expect(!p && old_cap_ != 0, 0)) p = find_in_old(key, h);
    return const_cast<value_type*>(p);
  }

  struct Probe {
    size_t idx;   // unified slot index
    bool found;   // key already present
  };

  // Find key; if absent, locate the slot to insert into (first deleted slot
  // wins over a later empty one so tombstones get recycled).
  // Only looks at the current table.
  Probe find_or_prepare(const key_type& key, uint64_t h) noexcept {
    const uint8_t hh = detail::H2(h);
    const size_t ng = cap_ / kWidth;
    const size_t home = detail::Fastrange(h, ng);
    size_t gi = home;
    size_t first_del = cap_;
    // Skip tombstone tracking entirely when there are none (common case).
    const bool track_del = (deleted_ != 0);
    for (size_t k = 0; k < ng; ++k) {
      if (gi >= ng) gi -= ng;
      detail::Group g = detail::Group::Load(ctrl_ + gi * kWidth);
      uint32_t m = g.Match(hh);
      while (m) {
        const unsigned j = detail::Ctzb(m);
        m &= m - 1;
        const size_t idx = gi * kWidth + j;
        if (equal_(slot_at(idx)->first, key)) return {idx, true};
      }
      if (track_del && first_del == cap_) {
        const uint32_t md = g.Match(detail::kDeleted);
        if (md) first_del = gi * kWidth + detail::Ctzb(md);
      }
      const uint32_t me = g.Match(detail::kEmpty);
      if (me) {
        size_t idx = gi * kWidth + detail::Ctzb(me);
        if (first_del != cap_) idx = first_del;
        // If the key lands past its home group, mark the home group so that
        // finds can early-out on a miss after checking just the home group.
        if (idx / kWidth != home) overflow_[home] |= detail::OverflowBit(h);
        return {idx, false};
      }
      ++gi;
    }
    return {first_del, false};  // reachable only if no empty slot exists
  }

  // During a migration, a new key whose old-table home is ahead of the
  // migration front goes into the old table. Then the new table is written
  // only near the front, in index order, so its pages fill in order and the
  // old pages behind the front can be released.
  // Returns an old-table index (>= mig_pos_), or old_cap_ if the key must go
  // to the new table.
  size_t old_insert_slot(uint64_t h) noexcept {
    const size_t ng = old_cap_ / kWidth;
    const size_t home = detail::Fastrange(h, ng);
    if (home * kWidth < mig_pos_) return old_cap_;
    for (size_t gi = home; gi < ng; ++gi) {  // no wrap: stay ahead of front
      const uint32_t me =
          detail::Group::Load(old_ctrl_ + gi * kWidth).Match(detail::kEmpty);
      if (me) {
        if (gi != home) old_overflow_[home] |= detail::OverflowBit(h);
        return gi * kWidth + detail::Ctzb(me);
      }
    }
    return old_cap_;
  }

  // Locate key for an insert. If absent, the returned slot is free.
  Probe locate_for_insert(const key_type& key, uint64_t h) noexcept {
    const Probe p = find_or_prepare(key, h);
    if (p.found || !is_migrating()) return p;
    const value_type* op = find_in_old(key, h);
    if (op) return {index_of(op), true};
    const size_t od = old_insert_slot(h);
    if (od != old_cap_) return {cap_ + od, false};
    return p;
  }

  // Mark a constructed slot as full.
  void commit_insert(size_t idx, uint64_t h) noexcept {
    if (idx < cap_) {
      if (ctrl_[idx] == detail::kDeleted) --deleted_;
      ctrl_[idx] = detail::H2(h);
    } else {
      old_ctrl_[idx - cap_] = detail::H2(h);
    }
    ++size_;
  }

  // Common insert path. construct(void* where) builds the value.
  template <typename Construct>
  std::pair<iterator, bool> insert_with(const key_type& key,
                                        Construct&& construct) {
    const uint64_t h = hash_mixed(key);
    if (__builtin_expect(growth_left_ != 0, 1)) {
      // Fast path: no growth, no migration. Most keys are absent and their
      // home group has an empty slot. (An empty slot in the home group also
      // proves that the key is absent; tombstones do not matter here.)
      const uint8_t hh = detail::H2(h);
      const size_t home = detail::Fastrange(h, cap_ / kWidth);
      const detail::Group g = detail::Group::Load(ctrl_ + home * kWidth);
      uint32_t m = g.Match(hh);
      while (m) {
        value_type* e = cur_slot(home * kWidth + detail::Ctzb(m));
        if (equal_(e->first, key)) return {iterator(this, e), false};
        m &= m - 1;
      }
      const uint32_t me = g.Match(detail::kEmpty);
      if (me) {
        const size_t idx = home * kWidth + detail::Ctzb(me);
        value_type* e = cur_slot(idx);
        construct(static_cast<void*>(e));
        ctrl_[idx] = hh;
        ++size_;
        --growth_left_;
        return {iterator(this, e), true};
      }
    } else {
      prepare_insert_slow();
    }
    const Probe p = locate_for_insert(key, h);
    if (!p.found) {
      construct(static_cast<void*>(slot_at(p.idx)));
      commit_insert(p.idx, h);
      reset_growth_left();
    }
    return {iterator(this, slot_at(p.idx)), !p.found};
  }

  // First empty slot for a hash (table is all-empty on entry paths that use
  // this, e.g. rehash/copy).
  size_t find_empty(uint64_t h) noexcept {
    const size_t ng = cap_ / kWidth;
    const size_t home = detail::Fastrange(h, ng);
    size_t gi = home;
    for (size_t k = 0; k < ng; ++k) {
      if (gi >= ng) gi -= ng;
      detail::Group g = detail::Group::Load(ctrl_ + gi * kWidth);
      const uint32_t me = g.Match(detail::kEmpty);
      if (me) {
        const size_t dst = gi * kWidth + detail::Ctzb(me);
        if (dst / kWidth != home) overflow_[home] |= detail::OverflowBit(h);
        return dst;
      }
      ++gi;
    }
    assert(false && "find_empty: no empty slot");
    return cap_;
  }

  static size_t align_cap(size_t n) noexcept {
    return (n + kWidth - 1) / kWidth * kWidth;
  }

  // Minimum capacity (multiple of kWidth) holding n elements at the current
  // max load factor: align(ceil(n / mlf)).
  size_t min_cap_for(size_t n) const noexcept {
    if (n == 0) return 0;
    const double need = static_cast<double>(n) / max_load_factor_;
    if (need > static_cast<double>(kMaxCapacity))
      return std::numeric_limits<size_t>::max();  // check_capacity throws
    size_t c = static_cast<size_t>(need);
    if (static_cast<double>(c) < need) ++c;  // ceil
    if (c < kWidth) c = kWidth;
    return align_cap(c);
  }

  // Growth by tiers of the current table size: 2x for small tables (few
  // moves per element), 1.5x and then 1.25x for large tables (less memory
  // waste and a lower growth peak).
  size_t grown_cap(size_t need) const noexcept {
    const uint64_t c = cap_;
    const uint64_t bytes = c * (sizeof(slot_type) + 1);
    uint64_t grown = bytes < SFLAT_GROW_2X_BELOW     ? c * 2
                     : bytes < SFLAT_GROW_1_5X_BELOW ? c + c / 2
                                                     : c + c / 4;
    if (grown < need) grown = need;
    if (grown > kMaxCapacity) {
      // Clamp to the largest capacity; if even that is too small,
      // check_capacity throws.
      if (need > kMaxCapacity) return need;
      grown = kMaxCapacity;
    }
    return align_cap(static_cast<size_t>(grown));
  }

  static void check_capacity(size_t cap) {
    if (cap > kMaxCapacity)
      throw std::length_error("sflat::flat_hash_map: too many elements");
  }

  size_t load_limit() const noexcept {
    return static_cast<size_t>(static_cast<double>(cap_) * max_load_factor_);
  }
  void update_limit() noexcept {
    limit_ = load_limit();
    reset_growth_left();
  }

  // Inserts left before the slow path must run (0 while migrating).
  void reset_growth_left() noexcept {
    const size_t used = size_ + deleted_;
    growth_left_ = (old_cap_ == 0 && used < limit_) ? limit_ - used : 0;
  }

  bool is_migrating() const noexcept { return old_cap_ != 0; }

  // ------------------------------------------------------------ table memory
  slot_type* alloc_slots(size_t n, bool huge) {
    if constexpr (kOwnMemory)
      return static_cast<slot_type*>(
          detail::PageAlloc(n * sizeof(slot_type), kSlotAlign, huge));
    else
      return slot_traits::allocate(slot_alloc_, n);
  }
  void free_slots(slot_type* p, size_t n) noexcept {
    if constexpr (kOwnMemory)
      detail::PageFree(p, n * sizeof(slot_type), kSlotAlign);
    else if (p)
      slot_traits::deallocate(slot_alloc_, p, n);
  }
  uint8_t* alloc_bytes(size_t n) {
    if constexpr (kOwnMemory)
      return static_cast<uint8_t*>(
          detail::PageAlloc(n, detail::kCacheLine, true));
    else
      return ctrl_traits::allocate(ctrl_alloc_, n);
  }
  void free_bytes(uint8_t* p, size_t n) noexcept {
    if constexpr (kOwnMemory)
      detail::PageFree(p, n, detail::kCacheLine);
    else if (p)
      ctrl_traits::deallocate(ctrl_alloc_, p, n);
  }
  // Zeroed overflow words for ng groups.
  using ovf_alloc = typename std::allocator_traits<Allocator>::template rebind_alloc<
      detail::OverflowWord>;
  using ovf_traits = std::allocator_traits<ovf_alloc>;
  detail::OverflowWord* alloc_overflow(size_t ng) {
    const size_t n = ng * sizeof(detail::OverflowWord);
    detail::OverflowWord* p;
    if constexpr (kOwnMemory) {
      p = static_cast<detail::OverflowWord*>(
          detail::PageAlloc(n, detail::kCacheLine, true));
    } else {
      ovf_alloc a(ctrl_alloc_);
      p = ovf_traits::allocate(a, ng);
    }
    std::memset(p, 0, n);
    return p;
  }
  void free_overflow(detail::OverflowWord* p, size_t ng) noexcept {
    if constexpr (kOwnMemory) {
      detail::PageFree(p, ng * sizeof(detail::OverflowWord), detail::kCacheLine);
    } else if (p) {
      ovf_alloc a(ctrl_alloc_);
      ovf_traits::deallocate(a, p, ng);
    }
  }

  // ------------------------------------------------------------ growth
  // Start incremental migration to a new table. The old table is kept and
  // migrated in small steps to avoid latency spikes.
  void begin_migration(size_t new_cap) {
    check_capacity(new_cap);
    new_cap = align_cap(new_cap);
    assert(!is_migrating());
    // The new slots start without huge pages; migrate_step enables them just
    // ahead of the front, so stray writes far ahead do not commit whole huge
    // pages.
    slot_type* new_slots = alloc_slots(new_cap, false);
    uint8_t* new_ctrl = alloc_bytes(new_cap);
    std::memset(new_ctrl, detail::kEmpty, new_cap);
    const size_t new_ng = new_cap / kWidth;
    detail::OverflowWord* new_overflow = alloc_overflow(new_ng);
    // Move current to old.
    old_slots_ = slots_;
    old_ctrl_ = ctrl_;
    old_overflow_ = overflow_;
    old_cap_ = cap_;
    mig_pos_ = 0;
    old_released_ = 0;
    huge_hint_ = 0;
    // Old tombstones are dropped by the migration.
    deleted_ = 0;
    // Install new.
    slots_ = new_slots;
    ctrl_ = new_ctrl;
    overflow_ = new_overflow;
    cap_ = new_cap;
    update_limit();
  }

  // Move one full old slot into the new table.
  void migrate_one(size_t old_idx) {
    value_type* s =
        reinterpret_cast<value_type*>(static_cast<void*>(&old_slots_[old_idx]));
    const uint64_t h = hash_mixed(s->first);
    // Fast path: an empty slot in the home group (the new table is at most
    // ~60% full during a migration).
    const size_t home = detail::Fastrange(h, cap_ / kWidth);
    const uint32_t me =
        detail::Group::Load(ctrl_ + home * kWidth).Match(detail::kEmpty);
    const size_t dst = me ? home * kWidth + detail::Ctzb(me) : find_empty(h);
    ::new (static_cast<void*>(&slots_[dst])) value_type(std::move(*s));
    s->~value_type();
    ctrl_[dst] = detail::H2(h);
  }

  // Migrate up to `batch` old slots (a multiple of kWidth), one group at a
  // time, then manage pages: release old slots behind the front, and ask for
  // huge pages in the new table up to just ahead of the front.
  void migrate_step(size_t batch = kMigrateBatch) {
    if (!is_migrating()) return;
    constexpr uint32_t kAll =
        static_cast<uint32_t>((uint64_t{1} << kWidth) - 1);
    const size_t stop = old_cap_ - mig_pos_ > batch ? mig_pos_ + batch : old_cap_;
    for (; mig_pos_ < stop; mig_pos_ += kWidth) {
      uint8_t* oc = old_ctrl_ + mig_pos_;
      const detail::Group g = detail::Group::Load(oc);
      uint32_t full = ~(g.Match(detail::kEmpty) | g.Match(detail::kDeleted)) & kAll;
      while (full) {
        migrate_one(mig_pos_ + detail::Ctzb(full));
        full &= full - 1;
      }
      // Mark the whole group deleted (not empty): finds that probe through it
      // must not stop here.
      std::memset(oc, detail::kDeleted, kWidth);
    }
    if (mig_pos_ >= old_cap_) {
      free_old();
      reset_growth_left();
      // Pages that were written before their huge page hint (rare) are
      // converted now.
      if constexpr (kOwnMemory)
        detail::PageHuge(slots_, cap_ * sizeof(slot_type), 0,
                         cap_ * sizeof(slot_type), true);
      huge_hint_ = 0;
      return;
    }
    if constexpr (kOwnMemory) manage_pages();
  }

  void manage_pages() noexcept {
    constexpr size_t sz = sizeof(slot_type);
    const size_t old_bytes = old_cap_ * sz;
    if (detail::IsMapped(old_bytes)) {
      const size_t done = mig_pos_ * sz / detail::kHugePage * detail::kHugePage;
      if (done > old_released_) {
        detail::PageRelease(old_slots_, old_bytes, old_released_, done);
        old_released_ = done;
      }
    }
    const size_t new_bytes = cap_ * sz;
    if (detail::IsMapped(new_bytes)) {
      // Front in the new table: the same relative position (Fastrange is
      // monotonic). Hint two huge pages ahead to cover probe spill.
      const size_t front =
          static_cast<size_t>((static_cast<__uint128_t>(mig_pos_) * cap_) /
                              old_cap_) * sz;
      const size_t want = front + 2 * detail::kHugePage;
      if (want > huge_hint_) {
        detail::PageHuge(slots_, new_bytes, huge_hint_, want, false);
        huge_hint_ = detail::RoundUp(want, detail::kHugePage);
      }
    }
  }

  // Slow part of insert: first allocation, growth, tombstone cleanup, and one
  // migration step.
  void prepare_insert_slow() {
    if (is_migrating()) {
      migrate_step();  // resets growth_left_ when the migration ends
      return;
    }
    if (cap_ == 0) {
      rehash_to(min_cap_for(1));
      return;
    }
    if (size_ + 1 > limit_) {
      begin_migration(grown_cap(min_cap_for(size_ + 1)));
      migrate_step();
    } else if (size_ + deleted_ + 1 > limit_) {
      rehash_to(cap_);  // recycle tombstones without growing
    }
  }

  // Synchronous rehash into a table of new_cap slots. Moves elements in index
  // order and releases the old slots behind the cursor, so the peak stays
  // near the size of the new table.
  void rehash_to(size_t new_cap) {
    // If migrating, finish it first to avoid losing unmigrated elements.
    if (is_migrating()) finish_migration();
    check_capacity(new_cap);
    new_cap = align_cap(new_cap);
    slot_type* new_slots = nullptr;
    uint8_t* new_ctrl = empty_ctrl();
    detail::OverflowWord* new_overflow = nullptr;
    if (new_cap) {
      new_slots = alloc_slots(new_cap, true);
      new_ctrl = alloc_bytes(new_cap);
      std::memset(new_ctrl, detail::kEmpty, new_cap);
      const size_t new_ng = new_cap / kWidth;
      new_overflow = alloc_overflow(new_ng);
      constexpr size_t sz = sizeof(slot_type);
      size_t released = 0;
      for (size_t i = 0; i < cap_; ++i) {
        if (detail::IsFull(ctrl_[i])) {
          value_type* s = slot_at(i);
          const uint64_t h = hash_mixed(s->first);
          // locate empty slot in the fresh table
          const size_t home = detail::Fastrange(h, new_ng);
          size_t gi = home;
          size_t dst = new_cap;
          for (size_t k = 0; k < new_ng; ++k) {
            if (gi >= new_ng) gi -= new_ng;
            detail::Group g = detail::Group::Load(new_ctrl + gi * kWidth);
            const uint32_t me = g.Match(detail::kEmpty);
            if (me) {
              dst = gi * kWidth + detail::Ctzb(me);
              break;
            }
            ++gi;
          }
          assert(dst != new_cap);
          ::new (static_cast<void*>(&new_slots[dst]))
              value_type(std::move(*s));
          destroy_slot(i);
          new_ctrl[dst] = detail::H2(h);
          if (dst / kWidth != home) new_overflow[home] |= detail::OverflowBit(h);
        }
        if constexpr (kOwnMemory) {
          if ((i + 1) * sz >= released + 64 * detail::kHugePage) {
            detail::PageRelease(slots_, cap_ * sz, released, (i + 1) * sz);
            released = (i + 1) * sz / detail::kHugePage * detail::kHugePage;
          }
        }
      }
    } else {
      assert(size_ == 0);
    }
    free_storage();
    slots_ = new_slots;
    ctrl_ = new_ctrl;
    overflow_ = new_overflow;
    cap_ = new_cap;
    deleted_ = 0;
    update_limit();
  }

  void free_storage() noexcept {
    if (cap_) {
      free_slots(slots_, cap_);
      free_bytes(ctrl_, cap_);
      free_overflow(overflow_, cap_ / kWidth);
    }
    free_old();
  }

  void free_old() noexcept {
    if (old_cap_) {
      free_slots(old_slots_, old_cap_);
      free_bytes(old_ctrl_, old_cap_);
      free_overflow(old_overflow_, old_cap_ / kWidth);
      old_slots_ = nullptr;
      old_ctrl_ = nullptr;
      old_overflow_ = nullptr;
      old_cap_ = 0;
      mig_pos_ = 0;
      old_released_ = 0;
    }
  }

  // --------------------------------------------------------------- modifiers
 public:
  void clear() noexcept {
    for (size_t i = 0; i < cap_ + old_cap_; ++i)
      if (detail::IsFull(ctrl_at(i))) destroy_slot(i);
    // Drop the old table of an ongoing migration too.
    free_old();
    if (cap_) {
      std::memset(ctrl_, detail::kEmpty, cap_);
      std::memset(overflow_, 0, cap_ / kWidth * sizeof(detail::OverflowWord));
    }
    size_ = 0;
    deleted_ = 0;
    reset_growth_left();
  }

  std::pair<iterator, bool> insert(const value_type& v) {
    return insert_with(v.first,
                       [&](void* p) { ::new (p) value_type(v); });
  }

  std::pair<iterator, bool> insert(value_type&& v) {
    return insert_with(v.first,
                       [&](void* p) { ::new (p) value_type(std::move(v)); });
  }

  template <typename P,
            typename = typename std::enable_if<
                std::is_constructible<value_type, P&&>::value &&
                !std::is_same<typename std::decay<P>::type, value_type>::value &&
                !std::is_same<typename std::decay<P>::type,
                              const value_type>::value>::type>
  std::pair<iterator, bool> insert(P&& obj) {
    value_type v(std::forward<P>(obj));
    return insert(std::move(v));
  }

  iterator insert(const_iterator hint, const value_type& v) {
    (void)hint;
    return insert(v).first;
  }
  iterator insert(const_iterator hint, value_type&& v) {
    (void)hint;
    return insert(std::move(v)).first;
  }

  template <typename InputIt>
  void insert(InputIt first, InputIt last) {
    for (; first != last; ++first) insert(*first);
  }

  void insert(std::initializer_list<value_type> il) {
    insert(il.begin(), il.end());
  }

  template <typename... Args>
  std::pair<iterator, bool> emplace(Args&&... args) {
    // Build the value first so the table is untouched on construction failure.
    value_type v(std::forward<Args>(args)...);
    return insert(std::move(v));
  }

  template <typename... Args>
  iterator emplace_hint(const_iterator hint, Args&&... args) {
    (void)hint;
    return emplace(std::forward<Args>(args)...).first;
  }

  template <typename... Args>
  std::pair<iterator, bool> try_emplace(const key_type& k, Args&&... args) {
    return insert_with(k, [&](void* p) {
      ::new (p) value_type(std::piecewise_construct, std::forward_as_tuple(k),
                           std::forward_as_tuple(std::forward<Args>(args)...));
    });
  }

  template <typename... Args>
  std::pair<iterator, bool> try_emplace(key_type&& k, Args&&... args) {
    return insert_with(k, [&](void* p) {
      ::new (p) value_type(std::piecewise_construct,
                           std::forward_as_tuple(std::move(k)),
                           std::forward_as_tuple(std::forward<Args>(args)...));
    });
  }

  template <typename... Args>
  iterator try_emplace(const_iterator hint, const key_type& k,
                       Args&&... args) {
    (void)hint;
    return try_emplace(k, std::forward<Args>(args)...).first;
  }

  template <typename... Args>
  iterator try_emplace(const_iterator hint, key_type&& k, Args&&... args) {
    (void)hint;
    return try_emplace(std::move(k), std::forward<Args>(args)...).first;
  }

  template <typename M>
  std::pair<iterator, bool> insert_or_assign(const key_type& k, M&& obj) {
    auto r = try_emplace(k, std::forward<M>(obj));
    if (!r.second) r.first->second = std::forward<M>(obj);
    return r;
  }

  template <typename M>
  std::pair<iterator, bool> insert_or_assign(key_type&& k, M&& obj) {
    auto r = try_emplace(std::move(k), std::forward<M>(obj));
    if (!r.second) r.first->second = std::forward<M>(obj);
    return r;
  }

  template <typename... Args>
  iterator insert_or_assign(const_iterator hint, const key_type& k,
                            Args&&... args) {
    (void)hint;
    return insert_or_assign(k, std::forward<Args>(args)...).first;
  }

  // Erase does not move other elements, so `it = m.erase(it)` loops are
  // safe also during a migration.
  iterator erase(const_iterator pos) {
    const size_t i = pos.index();
    assert(i < cap_ + old_cap_ && detail::IsFull(ctrl_at(i)));
    destroy_slot(i);
    if (i < cap_) {
      ctrl_[i] = detail::kDeleted;
      ++deleted_;
    } else {
      old_ctrl_[i - cap_] = detail::kDeleted;  // the migration skips it
    }
    --size_;
    return iterator(this, first_full_from(i + 1));
  }

  iterator erase(iterator pos) { return erase(const_iterator(pos)); }

  iterator erase(const_iterator first, const_iterator last) {
    while (first != last) first = erase(first);
    return iterator(this, const_cast<value_type*>(first.p_));
  }

  size_type erase(const key_type& key) {
    const iterator it = find(key);
    if (it == end()) return 0;
    erase(it);
    return 1;
  }

  // ------------------------------------------------------------------ lookup
  // find() never changes the map, also during a migration. Concurrent finds
  // from many threads are safe while no thread writes.
  iterator find(const key_type& key) {
    return iterator(this, find_ptr(key, hash_mixed(key)));
  }

  const_iterator find(const key_type& key) const {
    return const_iterator(this, find_ptr(key, hash_mixed(key)));
  }

  size_type count(const key_type& key) const {
    return find(key) == end() ? 0 : 1;
  }

  // Batch find for large tables. Processes keys in groups, prefetching home
  // buckets before the finds to overlap DRAM accesses. Faster than calling
  // find() in a loop when the table does not fit in cache.
  //
  // keys: array of n keys to look up.
  // out_found: array of n bools, set to true if key was found.
  // out_values: array of n mapped_type, set to the value if found
  //   (undefined if not found). May be nullptr.
  void find_batch(const key_type* keys, size_t n, bool* out_found,
                  mapped_type* out_values = nullptr) const {
    const size_t ng = cap_ / kWidth;
    constexpr size_t B = 64;
    for (size_t base = 0; base < n; base += B) {
      const size_t cnt = (base + B <= n) ? B : (n - base);
      uint64_t hashes[B];
      // Phase 1: compute hashes and prefetch home buckets.
      for (size_t i = 0; i < cnt; ++i) {
        const uint64_t h = hash_mixed(keys[base + i]);
        hashes[i] = h;
        if (ng) {
          const size_t home = detail::Fastrange(h, ng);
          __builtin_prefetch(ctrl_ + home * kWidth, 0, 0);
          __builtin_prefetch(slots_ + home * kWidth, 0, 0);
        }
      }
      // Phase 2: do the finds (data is now in flight from DRAM).
      for (size_t i = 0; i < cnt; ++i) {
        const value_type* e = find_ptr(keys[base + i], hashes[i]);
        out_found[base + i] = e != nullptr;
        if (e && out_values) out_values[base + i] = e->second;
      }
    }
  }

  bool contains(const key_type& key) const {  // C++20 convenience
    return find(key) != end();
  }

  std::pair<iterator, iterator> equal_range(const key_type& key) {
    const iterator it = find(key);
    if (it == end()) return {it, it};
    iterator nx = it;
    ++nx;
    return {it, nx};
  }

  std::pair<const_iterator, const_iterator> equal_range(
      const key_type& key) const {
    const const_iterator it = find(key);
    if (it == end()) return {it, it};
    const_iterator nx = it;
    ++nx;
    return {it, nx};
  }

  mapped_type& at(const key_type& key) {
    const iterator it = find(key);
    if (it == end()) throw std::out_of_range("flat_hash_map::at");
    return it->second;
  }

  const mapped_type& at(const key_type& key) const {
    const const_iterator it = find(key);
    if (it == end()) throw std::out_of_range("flat_hash_map::at");
    return it->second;
  }

  mapped_type& operator[](const key_type& key) {
    return try_emplace(key).first->second;
  }

  mapped_type& operator[](key_type&& key) {
    return try_emplace(std::move(key)).first->second;
  }

  // -------------------------------------------------------------- hash policy
  double load_factor() const noexcept {
    return cap_ ? static_cast<double>(size_) / static_cast<double>(cap_) : 0.0;
  }

  double max_load_factor() const noexcept { return max_load_factor_; }

  void max_load_factor(double ml) {
    assert(ml > 0.0 && ml < 1.0);
    max_load_factor_ = ml;
    update_limit();
    if (cap_ && size_ > limit_) rehash_to(min_cap_for(size_));
  }

  void rehash(size_type n) {
    const size_type want = min_cap_for(n > size_ ? n : size_);
    if (want != cap_ || deleted_ > 0 || is_migrating()) rehash_to(want);
  }

  void reserve(size_type n) { rehash(n); }

  // Optimize for steady-state finds: rebuild the table to remove tombstones.
  // Call this when no more inserts or erases are expected. This is equivalent
  // to rehash(capacity()) but also shrinks if the table is oversized.
  void optimize() {
    if (is_migrating()) finish_migration();
    if (size_ == 0) {
      clear();
      return;
    }
    // Rebuild at the minimal capacity that fits, removing tombstones.
    // This re-lays out elements and shortens probe chains.
    rehash_to(min_cap_for(size_));
  }

  void shrink_to_fit() {
    if (is_migrating()) finish_migration();
    rehash_to(min_cap_for(size_));
  }

  // ----------------------------------------------------------------- buckets
  size_type bucket_count() const noexcept { return cap_; }

  size_type max_bucket_count() const noexcept { return kMaxCapacity; }

  // Open addressing: a "bucket" holds at most one element.
  size_type bucket_size(size_type n) const {
    assert(n < cap_);
    return detail::IsFull(ctrl_[n]) ? 1 : 0;
  }

  size_type bucket(const key_type& key) const {
    // 64-bit fastrange (not the 32-bit hot-path one): bucket() is not
    // performance-critical and cap_ may exceed 2^32 on huge tables.
    const uint64_t h = hash_mixed(key);
    return cap_ ? static_cast<size_type>((static_cast<__uint128_t>(h) * cap_) >> 64)
                : 0;
  }

  local_iterator begin(size_type n) { return local_iterator(this, n, 0); }
  const_local_iterator begin(size_type n) const {
    return const_local_iterator(this, n, 0);
  }
  const_local_iterator cbegin(size_type n) const {
    return const_local_iterator(this, n, 0);
  }
  local_iterator end(size_type n) { return local_iterator(this, n, cap_); }
  const_local_iterator end(size_type n) const {
    return const_local_iterator(this, n, cap_);
  }
  const_local_iterator cend(size_type n) const {
    return const_local_iterator(this, n, cap_);
  }

  // ---------------------------------------------------------------- observers
  hasher hash_function() const { return hash_; }
  key_equal key_eq() const { return equal_; }
  allocator_type get_allocator() const {
    return allocator_type(slot_alloc_);
  }

 private:
  size_type bucket_index(const key_type& key) const {
    const uint64_t h = hash_mixed(key);
    return static_cast<size_type>((static_cast<__uint128_t>(h) * cap_) >> 64);
  }

  // ------------------------------------------------------------------- data
  slot_type* slots_ = nullptr;
  uint8_t* ctrl_ = empty_ctrl();  // size cap_; H2 / kEmpty / kDeleted
  detail::OverflowWord* overflow_ = nullptr;  // size cap_/kWidth; Bloom bits
  size_type cap_ = 0;
  size_type size_ = 0;
  size_type deleted_ = 0;  // tombstones in the current table
  size_type limit_ = 0;    // load_limit() of the current table
  size_type growth_left_ = 0;  // see reset_growth_left()
  double max_load_factor_ = kDefaultMaxLoadFactor;
  Hash hash_;
  KeyEqual equal_;
  slot_alloc slot_alloc_;
  ctrl_alloc ctrl_alloc_;
  // Incremental rehash state: when growing, old table is migrated to new
  // in small steps to avoid latency spikes.
  slot_type* old_slots_ = nullptr;
  uint8_t* old_ctrl_ = nullptr;
  detail::OverflowWord* old_overflow_ = nullptr;
  size_type old_cap_ = 0;
  size_type mig_pos_ = 0;       // next old slot to migrate
  size_type old_released_ = 0;  // old slot bytes given back to the kernel
  size_type huge_hint_ = 0;     // new slot bytes with the huge page hint
};

template <typename Key, typename T, typename Hash, typename KeyEqual,
          typename Allocator>
bool operator==(const flat_hash_map<Key, T, Hash, KeyEqual, Allocator>& a,
                const flat_hash_map<Key, T, Hash, KeyEqual, Allocator>& b) {
  if (a.size() != b.size()) return false;
  for (const auto& kv : a) {
    const auto it = b.find(kv.first);
    if (it == b.end() || it->second != kv.second) return false;
  }
  return true;
}

template <typename Key, typename T, typename Hash, typename KeyEqual,
          typename Allocator>
bool operator!=(const flat_hash_map<Key, T, Hash, KeyEqual, Allocator>& a,
                const flat_hash_map<Key, T, Hash, KeyEqual, Allocator>& b) {
  return !(a == b);
}

template <typename Key, typename T, typename Hash, typename KeyEqual,
          typename Allocator>
void swap(flat_hash_map<Key, T, Hash, KeyEqual, Allocator>& a,
          flat_hash_map<Key, T, Hash, KeyEqual, Allocator>& b) noexcept(
      noexcept(a.swap(b))) {
  a.swap(b);
}

}  // namespace sflat
