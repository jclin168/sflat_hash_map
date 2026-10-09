#pragma once
//
// sflat::flat_hash_map
// =====================
// An open-addressing hash map with an std::unordered_map-compatible interface
// (C++17), designed for very large tables:
//
//  * SIMD group probing (SSE2 x16, AVX2 x32 when available) à la SwissTable.
//  * Growth factor 1.5x instead of 2x, and capacities are any multiple of the
//    group width (not restricted to powers of two) thanks to Lemire's
//    "fastrange" mapping.  This bounds memory waste for 100M+ entry tables.
//  * Hot paths are insert() and find(); erase() uses tombstones and
//    iteration is a plain linear scan.
//  * The user hash is passed through a splitmix64-style finalizer so the map
//    stays robust even with weak hashes (e.g. std::hash<uint64_t> identity on
//    sequential keys).
//
// Layout: one control byte per slot (SwissTable-style: H2 in [0,0x7F],
// 0xFF = empty, 0xFE = deleted) stored in a dense array, followed by the
// slots (std::pair<const Key, T> in aligned raw storage).

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <memory>
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

namespace sflat {
namespace detail {

inline constexpr uint8_t kEmpty = 0xFF;    // never used as H2
inline constexpr uint8_t kDeleted = 0xFE;  // tombstone
inline constexpr bool IsFull(uint8_t c) noexcept { return c < 0xFE; }

// Low 8 bits of the (mixed) hash, remapped from [0,256) to [0,253] to avoid
// colliding with kEmpty/kDeleted. Gives ~2x fewer spurious H2 matches than
// a 7-bit fingerprint.
inline uint8_t H2(uint64_t h) noexcept {
  uint8_t x = static_cast<uint8_t>(h);
  x -= static_cast<uint8_t>((x >= 0xFE) ? 2 : 0);
  return x;
}

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
// Uses bits [32,64) of the mixed hash, disjoint from H2's bits [0,7).
inline size_t Fastrange(uint64_t h, size_t n) noexcept {
  // assert(n < (1ULL << 32));
  return (size_t)(((uint64_t)(uint32_t)n * (uint32_t)(h >> 32)) >> 32);
}

// Per-group overflow Bloom bit: when a key's home group is full and the key
// spills to a later group, set this bit on the home group. Find can then
// terminate a miss after the home group if the bit is clear.
// Uses hash bits [8,11), disjoint from H2 bits [0,7) and position bits [32,64).
inline uint8_t OverflowBit(uint64_t h) noexcept {
  return static_cast<uint8_t>(1u << ((h >> 8) & 7u));
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
  // 1.5x growth: growth by 3/2 instead of 2/1.
  static constexpr size_t kGrowNum = 3;
  static constexpr size_t kGrowDen = 2;

 public:
  // ---------------------------------------------------------------- iterators
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
        : map_(o.map_), idx_(o.idx_) {}

    reference operator*() const noexcept { return *map_->slot_at(idx_); }
    pointer operator->() const noexcept { return map_->slot_at(idx_); }

    iterator_impl& operator++() noexcept {
      ++idx_;
      seek();
      return *this;
    }
    iterator_impl operator++(int) noexcept {
      iterator_impl c(*this);
      ++*this;
      return c;
    }

    size_t index() const noexcept { return idx_; }

    template <bool O>
    bool operator==(const iterator_impl<O>& o) const noexcept {
      return map_ == o.map_ && idx_ == o.idx_;
    }
    template <bool O>
    bool operator!=(const iterator_impl<O>& o) const noexcept {
      return !(*this == o);
    }

   private:
    friend class flat_hash_map;
    template <bool>
    friend class iterator_impl;
    using map_ptr = typename std::conditional<IsConst, const flat_hash_map*,
                                              flat_hash_map*>::type;

    iterator_impl(map_ptr m, size_t i) noexcept : map_(m), idx_(i) {
      if (map_) seek();
    }
    void seek() noexcept {
      while (idx_ < map_->cap_ && !detail::IsFull(map_->ctrl_[idx_])) ++idx_;
    }

    map_ptr map_ = nullptr;
    size_t idx_ = 0;
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
      : hash_(other.hash_),
        equal_(other.equal_),
        max_load_factor_(other.max_load_factor_),
        slot_alloc_(slot_traits::select_on_container_copy_construction(
            other.slot_alloc_)),
        ctrl_alloc_(ctrl_traits::select_on_container_copy_construction(
            other.ctrl_alloc_)) {
    if (other.cap_) {
      rehash_to(other.cap_);
      for (size_t i = 0; i < other.cap_; ++i) {
        if (detail::IsFull(other.ctrl_[i])) {
          const value_type* s = other.slot_at(i);
          uint64_t h = hash_mixed(s->first);
          size_t dst = find_empty(h);
          construct_slot(dst, *s);
          ctrl_[dst] = detail::H2(h);
        }
      }
      size_ = other.size_;
    }
  }

  flat_hash_map(const flat_hash_map& other, const Allocator& alloc)
      : hash_(other.hash_),
        equal_(other.equal_),
        max_load_factor_(other.max_load_factor_),
        slot_alloc_(alloc),
        ctrl_alloc_(alloc) {
    if (other.cap_) {
      rehash_to(other.cap_);
      for (size_t i = 0; i < other.cap_; ++i) {
        if (detail::IsFull(other.ctrl_[i])) {
          const value_type* s = other.slot_at(i);
          uint64_t h = hash_mixed(s->first);
          size_t dst = find_empty(h);
          construct_slot(dst, *s);
          ctrl_[dst] = detail::H2(h);
        }
      }
      size_ = other.size_;
    }
  }

  flat_hash_map(flat_hash_map&& other) noexcept
      : slots_(other.slots_),
        ctrl_(other.ctrl_),
        overflow_(other.overflow_),
        cap_(other.cap_),
        size_(other.size_),
        deleted_(other.deleted_),
        max_load_factor_(other.max_load_factor_),
        hash_(std::move(other.hash_)),
        equal_(std::move(other.equal_)),
        slot_alloc_(std::move(other.slot_alloc_)),
        ctrl_alloc_(std::move(other.ctrl_alloc_)) {
    other.slots_ = nullptr;
    other.ctrl_ = nullptr;
    other.overflow_ = nullptr;
    other.cap_ = 0;
    other.size_ = 0;
    other.deleted_ = 0;
  }

  flat_hash_map(flat_hash_map&& other, const Allocator& alloc)
      : hash_(std::move(other.hash_)),
        equal_(std::move(other.equal_)),
        max_load_factor_(other.max_load_factor_),
        slot_alloc_(alloc),
        ctrl_alloc_(alloc) {
    if (alloc == other.get_allocator()) {
      slots_ = other.slots_;
      ctrl_ = other.ctrl_;
      overflow_ = other.overflow_;
      cap_ = other.cap_;
      size_ = other.size_;
      deleted_ = other.deleted_;
      other.slots_ = nullptr;
      other.ctrl_ = nullptr;
      other.overflow_ = nullptr;
      other.cap_ = 0;
      other.size_ = 0;
      other.deleted_ = 0;
    } else if (other.cap_) {
      rehash_to(other.cap_);
      for (size_t i = 0; i < other.cap_; ++i) {
        if (detail::IsFull(other.ctrl_[i])) {
          value_type* s = other.slot_at(i);
          uint64_t h = hash_mixed(s->first);
          size_t dst = find_empty(h);
          construct_slot(dst, std::move(*s));
          ctrl_[dst] = detail::H2(h);
        }
      }
      size_ = other.size_;
      other.clear();
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
        slots_ = other.slots_;
        ctrl_ = other.ctrl_;
        overflow_ = other.overflow_;
        cap_ = other.cap_;
        size_ = other.size_;
        deleted_ = other.deleted_;
        other.slots_ = nullptr;
        other.ctrl_ = nullptr;
        other.overflow_ = nullptr;
        other.cap_ = 0;
        other.size_ = 0;
        other.deleted_ = 0;
      } else {
        // Allocators don't propagate: move elements one by one.
        if (other.cap_) {
          rehash_to(other.cap_);
          for (size_t i = 0; i < other.cap_; ++i) {
            if (detail::IsFull(other.ctrl_[i])) {
              value_type* s = other.slot_at(i);
              uint64_t h = hash_mixed(s->first);
              size_t dst = find_empty(h);
              construct_slot(dst, std::move(*s));
              ctrl_[dst] = detail::H2(h);
            }
          }
          size_ = other.size_;
          other.clear();
        }
      }
      hash_ = std::move(other.hash_);
      equal_ = std::move(other.equal_);
      max_load_factor_ = other.max_load_factor_;
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
    swap(max_load_factor_, other.max_load_factor_);
    swap(hash_, other.hash_);
    swap(equal_, other.equal_);
    swap(old_slots_, other.old_slots_);
    swap(old_ctrl_, other.old_ctrl_);
    swap(old_overflow_, other.old_overflow_);
    swap(old_cap_, other.old_cap_);
    swap(mig_pos_, other.mig_pos_);
    if (slot_traits::propagate_on_container_swap::value) {
      swap(slot_alloc_, other.slot_alloc_);
      swap(ctrl_alloc_, other.ctrl_alloc_);
    }
  }

  // ------------------------------------------------------------------ lookup
  // Finish any ongoing migration (for iteration; not a hot path).
  void finish_migration() {
    while (is_migrating()) {
      // Migrate in larger batches to finish quickly.
      for (int i = 0; i < 16 && is_migrating(); ++i) migrate_step();
    }
  }

  iterator begin() noexcept {
    // Iteration requires a single table; finish migration first.
    // (const_cast: begin() is non-const, so this is fine.)
    finish_migration();
    return iterator(this, 0);
  }
  const_iterator begin() const noexcept {
    const_cast<flat_hash_map*>(this)->finish_migration();
    return const_iterator(this, 0);
  }
  const_iterator cbegin() const noexcept { return const_iterator(this, 0); }
  iterator end() noexcept { return iterator(this, cap_); }
  const_iterator end() const noexcept { return const_iterator(this, cap_); }
  const_iterator cend() const noexcept { return const_iterator(this, cap_); }

  bool empty() const noexcept { return size_ == 0; }
  size_type size() const noexcept { return size_; }
  size_type max_size() const noexcept {
    return (std::numeric_limits<size_type>::max() / sizeof(slot_type)) - 1;
  }

  // ------------------------------------------------------------- core probes
 private:
  value_type* slot_at(size_t i) noexcept {
    return reinterpret_cast<value_type*>(static_cast<void*>(&slots_[i]));
  }
  const value_type* slot_at(size_t i) const noexcept {
    return reinterpret_cast<const value_type*>(
        static_cast<const void*>(&slots_[i]));
  }

  template <typename... Args>
  value_type* construct_slot(size_t i, Args&&... args) {
    return ::new (static_cast<void*>(&slots_[i]))
        value_type(std::forward<Args>(args)...);
  }
  void destroy_slot(size_t i) noexcept { slot_at(i)->~value_type(); }

  uint64_t hash_mixed(const key_type& k) const {
    return detail::MixHash(static_cast<uint64_t>(hash_(k)));
  }

  // Index of key in the given table, or cap if absent.
  static size_t find_index_in(const key_type& key, uint64_t h,
                              const uint8_t* ctrl, const slot_type* slots,
                              size_t cap, const uint8_t* overflow,
                              const KeyEqual& equal) noexcept {
    if (cap == 0) return 0;
    const uint8_t hh = detail::H2(h);
    const size_t ng = cap / kWidth;
    const size_t home = detail::Fastrange(h, ng);
    {
      detail::Group g = detail::Group::Load(ctrl + home * kWidth);
      uint32_t m = g.Match(hh);
      while (m) {
        const unsigned j = detail::Ctzb(m);
        m &= m - 1;
        const size_t idx = home * kWidth + j;
        if (equal(reinterpret_cast<const value_type*>(&slots[idx])->first, key))
          return idx;
      }
      if (g.Match(detail::kEmpty)) return cap;
      if (!(overflow[home] & detail::OverflowBit(h))) return cap;
    }
    size_t gi = home + 1;
    for (size_t k = 1; k < ng; ++k) {
      if (gi >= ng) gi -= ng;
      detail::Group g = detail::Group::Load(ctrl + gi * kWidth);
      uint32_t m = g.Match(hh);
      while (m) {
        const unsigned j = detail::Ctzb(m);
        m &= m - 1;
        const size_t idx = gi * kWidth + j;
        if (equal(reinterpret_cast<const value_type*>(&slots[idx])->first, key))
          return idx;
      }
      if (g.Match(detail::kEmpty)) return cap;
      ++gi;
    }
    return cap;
  }

  // Index of key, or cap_ if absent.
  size_t find_index(const key_type& key, uint64_t h) const noexcept {
    return find_index_in(key, h, ctrl_, slots_, cap_, overflow_, equal_);
  }

  struct Probe {
    size_t idx;   // slot index (cap_ if table has no usable slot)
    bool found;   // key already present
  };

  // Find key; if absent, locate the slot to insert into (first deleted slot
  // wins over a later empty one so tombstones get recycled).
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
    size_t c = static_cast<size_t>(need);
    if (static_cast<double>(c) < need) ++c;  // ceil
    if (c < kWidth) c = kWidth;
    return align_cap(c);
  }

  // 1.5x growth (never 2x): keeps peak memory bounded for huge tables.
  size_t grown_cap(size_t need) const noexcept {
    size_t grown = cap_ + cap_ / kGrowDen;  // cap_ * 1.5
    if (grown < cap_) grown = std::numeric_limits<size_t>::max();  // overflow
    return align_cap(grown > need ? grown : need);
  }

  size_t load_limit() const noexcept {
    return static_cast<size_t>(static_cast<double>(cap_) * max_load_factor_);
  }

  bool is_migrating() const noexcept { return old_cap_ != 0; }

  // Start incremental migration to a new table. Old table is kept and
  // migrated in small steps to avoid latency/memory spikes.
  void begin_migration(size_t new_cap) {
    new_cap = align_cap(new_cap);
    assert(new_cap / kWidth < (1ULL << 32));
    assert(!is_migrating());
    // Allocate new tables.
    slot_type* new_slots = slot_traits::allocate(slot_alloc_, new_cap);
    uint8_t* new_ctrl = ctrl_traits::allocate(ctrl_alloc_, new_cap);
    std::memset(new_ctrl, detail::kEmpty, new_cap);
    const size_t new_ng = new_cap / kWidth;
    uint8_t* new_overflow = ctrl_traits::allocate(ctrl_alloc_, new_ng);
    std::memset(new_overflow, 0, new_ng);
    // Move current to old.
    old_slots_ = slots_; old_ctrl_ = ctrl_; old_overflow_ = overflow_;
    old_cap_ = cap_;
    mig_pos_ = 0;
    // Install new.
    slots_ = new_slots; ctrl_ = new_ctrl; overflow_ = new_overflow;
    cap_ = new_cap;
    deleted_ = 0;
  }

  // Migrate a small batch of slots from old to new table.
  void migrate_step() {
    if (!is_migrating()) return;
    const size_t batch = 64;  // slots per step
    const size_t new_ng = cap_ / kWidth;
    for (size_t i = 0; i < batch && mig_pos_ < old_cap_; ++i, ++mig_pos_) {
      if (!detail::IsFull(old_ctrl_[mig_pos_])) continue;
      migrate_one(mig_pos_);
      // migrate_one doesn't advance mig_pos_, so we need to handle it.
      // Actually, migrate_one is for on-demand; here we do it inline.
    }
    if (mig_pos_ >= old_cap_) {
      free_old();
    }
  }

  // Migrate a single old slot to the new table.
  void migrate_one(size_t old_idx) {
    assert(is_migrating() && old_idx < old_cap_);
    if (!detail::IsFull(old_ctrl_[old_idx])) return;
    const value_type* s = reinterpret_cast<const value_type*>(&old_slots_[old_idx]);
    const uint64_t h = hash_mixed(s->first);
    const size_t new_ng = cap_ / kWidth;
    const size_t home = detail::Fastrange(h, new_ng);
    size_t gi = home;
    size_t dst = cap_;
    for (size_t k = 0; k < new_ng; ++k) {
      if (gi >= new_ng) gi -= new_ng;
      detail::Group g = detail::Group::Load(ctrl_ + gi * kWidth);
      const uint32_t me = g.Match(detail::kEmpty);
      if (me) { dst = gi * kWidth + detail::Ctzb(me); break; }
      ++gi;
    }
    assert(dst != cap_);
    ::new (static_cast<void*>(&slots_[dst])) value_type(std::move(*const_cast<value_type*>(s)));
    const_cast<value_type*>(s)->~value_type();
    ctrl_[dst] = detail::H2(h);
    if (dst / kWidth != home) overflow_[home] |= detail::OverflowBit(h);
    // Mark as deleted (not empty) to preserve probing invariant in old table.
    old_ctrl_[old_idx] = detail::kDeleted;
  }

  void grow_for_insert() {
    if (cap_ == 0) {
      rehash_to(min_cap_for(1));
      return;
    }
    const size_t limit = load_limit();
    if (size_ + 1 > limit) {
      // Use incremental migration to avoid latency/memory spikes.
      if (!is_migrating()) {
        begin_migration(grown_cap(min_cap_for(size_ + 1)));
      }
      // If already migrating, just continue (migration will complete).
    } else if (size_ + deleted_ + 1 > limit) {
      rehash_to(cap_);  // recycle tombstones without growing
    }
  }

  void rehash_to(size_t new_cap) {
    // If migrating, finish it first to avoid losing unmigrated elements.
    if (is_migrating()) finish_migration();
    new_cap = align_cap(new_cap);
    slot_type* new_slots = nullptr;
    uint8_t* new_ctrl = nullptr;
    uint8_t* new_overflow = nullptr;
    if (new_cap) {
      // Hot-path Fastrange is 32-bit; 4G groups = 137G slots is far beyond
      // any practical table (100B entries need ~3.6G groups).
      assert(new_cap / kWidth < (1ULL << 32));
      new_slots = slot_traits::allocate(slot_alloc_, new_cap);
      new_ctrl = ctrl_traits::allocate(ctrl_alloc_, new_cap);
      std::memset(new_ctrl, detail::kEmpty, new_cap);
      const size_t new_ng = new_cap / kWidth;
      new_overflow = ctrl_traits::allocate(ctrl_alloc_, new_ng);
      std::memset(new_overflow, 0, new_ng);
      for (size_t i = 0; i < cap_; ++i) {
        if (!detail::IsFull(ctrl_[i])) continue;
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
    } else {
      assert(size_ == 0);
    }
    free_storage();
    slots_ = new_slots;
    ctrl_ = new_ctrl;
    overflow_ = new_overflow;
    cap_ = new_cap;
    deleted_ = 0;
  }

  void free_storage() noexcept {
    if (cap_) {
      slot_traits::deallocate(slot_alloc_, slots_, cap_);
      ctrl_traits::deallocate(ctrl_alloc_, ctrl_, cap_);
      ctrl_traits::deallocate(ctrl_alloc_, overflow_, cap_ / kWidth);
    }
    free_old();
  }

  void free_old() noexcept {
    if (old_cap_) {
      slot_traits::deallocate(slot_alloc_, old_slots_, old_cap_);
      ctrl_traits::deallocate(ctrl_alloc_, old_ctrl_, old_cap_);
      ctrl_traits::deallocate(ctrl_alloc_, old_overflow_, old_cap_ / kWidth);
      old_slots_ = nullptr; old_ctrl_ = nullptr; old_overflow_ = nullptr;
      old_cap_ = 0; mig_pos_ = 0;
    }
  }

  // --------------------------------------------------------------- modifiers
 public:
  void clear() noexcept {
    for (size_t i = 0; i < cap_; ++i)
      if (detail::IsFull(ctrl_[i])) destroy_slot(i);
    if (cap_) {
      std::memset(ctrl_, detail::kEmpty, cap_);
      std::memset(overflow_, 0, cap_ / kWidth);
    }
    size_ = 0;
    deleted_ = 0;
  }

  std::pair<iterator, bool> insert(const value_type& v) {
    const uint64_t h = hash_mixed(v.first);
    grow_for_insert();
    const Probe p = find_or_prepare(v.first, h);
    if (p.found) {
      migrate_step();
      return {iterator(this, p.idx), false};
    }
    if (is_migrating()) {
      const size_t oi = find_index_in(v.first, h, old_ctrl_, old_slots_, old_cap_,
                                     old_overflow_, equal_);
      if (oi != old_cap_) {
        migrate_one(oi);
        migrate_step();
        const size_t ni = find_index(v.first, h);
        return {iterator(this, ni), false};
      }
    }
    auto res = finish_insert(p, h, v);
    migrate_step();
    return res;
  }

  std::pair<iterator, bool> insert(value_type&& v) {
    const uint64_t h = hash_mixed(v.first);
    grow_for_insert();
    const Probe p = find_or_prepare(v.first, h);
    if (p.found) {
      migrate_step();
      return {iterator(this, p.idx), false};
    }
    if (is_migrating()) {
      const size_t oi = find_index_in(v.first, h, old_ctrl_, old_slots_, old_cap_,
                                     old_overflow_, equal_);
      if (oi != old_cap_) {
        migrate_one(oi);
        migrate_step();
        const size_t ni = find_index(v.first, h);
        return {iterator(this, ni), false};
      }
    }
    auto res = finish_insert(p, h, std::move(v));
    migrate_step();
    return res;
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
    const uint64_t h = hash_mixed(k);
    grow_for_insert();
    const Probe p = find_or_prepare(k, h);
    if (p.found) {
      migrate_step();
      return {iterator(this, p.idx), false};
    }
    // During migration, check old table for duplicates.
    if (is_migrating()) {
      const size_t oi = find_index_in(k, h, old_ctrl_, old_slots_, old_cap_,
                                     old_overflow_, equal_);
      if (oi != old_cap_) {
        // Found in old: migrate on-demand, then return the new position.
        migrate_one(oi);
        migrate_step();
        const size_t ni = find_index(k, h);
        return {iterator(this, ni), false};
      }
    }
    const bool reused = ctrl_[p.idx] == detail::kDeleted;
    construct_slot(p.idx, std::piecewise_construct, std::forward_as_tuple(k),
                   std::forward_as_tuple(std::forward<Args>(args)...));
    ctrl_[p.idx] = detail::H2(h);
    ++size_;
    if (reused) --deleted_;
    migrate_step();
    return {iterator(this, p.idx), true};
  }

  template <typename... Args>
  std::pair<iterator, bool> try_emplace(key_type&& k, Args&&... args) {
    const uint64_t h = hash_mixed(k);
    grow_for_insert();
    const Probe p = find_or_prepare(k, h);
    if (p.found) {
      migrate_step();
      return {iterator(this, p.idx), false};
    }
    if (is_migrating()) {
      const size_t oi = find_index_in(k, h, old_ctrl_, old_slots_, old_cap_,
                                     old_overflow_, equal_);
      if (oi != old_cap_) {
        migrate_one(oi);
        migrate_step();
        const size_t ni = find_index(k, h);
        return {iterator(this, ni), false};
      }
    }
    const bool reused = ctrl_[p.idx] == detail::kDeleted;
    construct_slot(p.idx, std::piecewise_construct,
                   std::forward_as_tuple(std::move(k)),
                   std::forward_as_tuple(std::forward<Args>(args)...));
    ctrl_[p.idx] = detail::H2(h);
    ++size_;
    if (reused) --deleted_;
    migrate_step();
    return {iterator(this, p.idx), true};
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
    const uint64_t h = hash_mixed(k);
    grow_for_insert();
    const Probe p = find_or_prepare(k, h);
    if (p.found) {
      slot_at(p.idx)->second = std::forward<M>(obj);
      migrate_step();
      return {iterator(this, p.idx), false};
    }
    if (is_migrating()) {
      const size_t oi = find_index_in(k, h, old_ctrl_, old_slots_, old_cap_,
                                     old_overflow_, equal_);
      if (oi != old_cap_) {
        migrate_one(oi);
        migrate_step();
        const size_t ni = find_index(k, h);
        slot_at(ni)->second = std::forward<M>(obj);
        return {iterator(this, ni), false};
      }
    }
    const bool reused = ctrl_[p.idx] == detail::kDeleted;
    construct_slot(p.idx, std::piecewise_construct, std::forward_as_tuple(k),
                   std::forward_as_tuple(std::forward<M>(obj)));
    ctrl_[p.idx] = detail::H2(h);
    ++size_;
    if (reused) --deleted_;
    migrate_step();
    return {iterator(this, p.idx), true};
  }

  template <typename M>
  std::pair<iterator, bool> insert_or_assign(key_type&& k, M&& obj) {
    const uint64_t h = hash_mixed(k);
    grow_for_insert();
    const Probe p = find_or_prepare(k, h);
    if (p.found) {
      slot_at(p.idx)->second = std::forward<M>(obj);
      return {iterator(this, p.idx), false};
    }
    const bool reused = ctrl_[p.idx] == detail::kDeleted;
    construct_slot(p.idx, std::piecewise_construct,
                   std::forward_as_tuple(std::move(k)),
                   std::forward_as_tuple(std::forward<M>(obj)));
    ctrl_[p.idx] = detail::H2(h);
    ++size_;
    if (reused) --deleted_;
    return {iterator(this, p.idx), true};
  }

  template <typename... Args>
  iterator insert_or_assign(const_iterator hint, const key_type& k,
                            Args&&... args) {
    (void)hint;
    return insert_or_assign(k, std::forward<Args>(args)...).first;
  }

 private:
  template <typename V>
  std::pair<iterator, bool> finish_insert(const Probe& p, uint64_t h,
                                          V&& v) {
    const bool reused = ctrl_[p.idx] == detail::kDeleted;
    construct_slot(p.idx, std::forward<V>(v));
    ctrl_[p.idx] = detail::H2(h);
    ++size_;
    if (reused) --deleted_;
    return {iterator(this, p.idx), true};
  }

 public:
  iterator erase(const_iterator pos) {
    const size_t i = pos.index();
    assert(i < cap_ && detail::IsFull(ctrl_[i]));
    destroy_slot(i);
    ctrl_[i] = detail::kDeleted;
    --size_;
    ++deleted_;
    migrate_step();
    return iterator(this, i + 1);
  }

  iterator erase(iterator pos) { return erase(const_iterator(pos)); }

  iterator erase(const_iterator first, const_iterator last) {
    while (first != last) first = erase(first);
    return iterator(this, first.index());
  }

  size_type erase(const key_type& key) {
    if (cap_ == 0) return 0;
    const iterator it = find(key);
    if (it == end()) return 0;
    erase(it);
    migrate_step();
    return 1;
  }

  // ------------------------------------------------------------------ lookup
  iterator find(const key_type& key) {
    if (cap_ == 0) return end();
    const uint64_t h = hash_mixed(key);
    size_t i = find_index(key, h);
    if (i != cap_) return iterator(this, i);
    if (is_migrating()) {
      // Check old table; if found, migrate it on-demand.
      const size_t oi = find_index_in(key, h, old_ctrl_, old_slots_, old_cap_,
                                     old_overflow_, equal_);
      if (oi != old_cap_) {
        migrate_one(oi);
        // Now it should be in the new table.
        i = find_index(key, h);
        if (i != cap_) return iterator(this, i);
      }
      // Also do a background migration step to make progress.
      migrate_step();
    }
    return end();
  }

  const_iterator find(const key_type& key) const {
    if (cap_ == 0) return end();
    const uint64_t h = hash_mixed(key);
    size_t i = find_index(key, h);
    if (i != cap_) return const_iterator(this, i);
    if (is_migrating()) {
      const size_t oi = find_index_in(key, h, old_ctrl_, old_slots_, old_cap_,
                                     old_overflow_, equal_);
      if (oi != old_cap_) {
        // Const version can't migrate; cast away constness for on-demand.
        const_cast<flat_hash_map*>(this)->migrate_one(oi);
        i = find_index(key, h);
        if (i != cap_) return const_iterator(this, i);
      }
    }
    return end();
  }

  size_type count(const key_type& key) const {
    return find(key) == end() ? 0 : 1;
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
    if (cap_ && size_ > load_limit()) rehash_to(min_cap_for(size_));
  }

  void rehash(size_type n) {
    const size_type want = min_cap_for(n > size_ ? n : size_);
    if (want != cap_ || deleted_ > 0) rehash_to(want);
  }

  void reserve(size_type n) { rehash(n); }

  // ----------------------------------------------------------------- buckets
  size_type bucket_count() const noexcept { return cap_; }

  size_type max_bucket_count() const noexcept {
    return max_size() < cap_ ? 0 : max_size();
  }

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
  uint8_t* ctrl_ = nullptr;  // size cap_; H2 / kEmpty / kDeleted
  uint8_t* overflow_ = nullptr;  // size cap_/kWidth; per-group overflow Bloom bits
  size_type cap_ = 0;
  size_type size_ = 0;
  size_type deleted_ = 0;  // tombstones
  double max_load_factor_ = kDefaultMaxLoadFactor;
  Hash hash_;
  KeyEqual equal_;
  slot_alloc slot_alloc_;
  ctrl_alloc ctrl_alloc_;
  // Incremental rehash state: when growing, old table is migrated to new
  // in small steps to avoid latency/memory spikes.
  slot_type* old_slots_ = nullptr;
  uint8_t* old_ctrl_ = nullptr;
  uint8_t* old_overflow_ = nullptr;
  size_type old_cap_ = 0;
  size_type mig_pos_ = 0;  // next old slot to migrate
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
