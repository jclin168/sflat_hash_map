// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jian-Cheng Lin
#pragma once
//
// sflat::flat_hash_set
// =====================
// The set counterpart of sflat::flat_hash_map, with an
// std::unordered_set-compatible interface (C++17). Same table: SIMD group
// probing, control bytes plus per-group overflow Bloom words, tiered growth
// (2x, 1.5x, 1.25x) with incremental migration, THP and page release on
// Linux. All of that, and the SFLAT_* configuration macros, come from
// flat_hash_map.hpp; this header only adds the container class.
//
// Differences from flat_hash_map: each slot stores the key itself (no
// mapped value), elements are immutable (iterator and const_iterator both
// give const references), and the map-only calls (operator[], at,
// try_emplace, insert_or_assign) do not exist. The interface is that of
// std::unordered_set only: find_batch() of the map is not offered.
//
// The class body is derived from flat_hash_map; keep the two in step when
// changing the table logic.

#include "flat_hash_map.hpp"

namespace sflat {

template <typename Key, typename Hash = std::hash<Key>,
          typename KeyEqual = std::equal_to<Key>,
          typename Allocator = std::allocator<Key>>
class flat_hash_set {
 public:
  using key_type = Key;
  using value_type = Key;
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

  using Group = detail::Group;
  using Mask = Group::Mask;
  static constexpr size_t kWidth = Group::kWidth;
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
  // With std::allocator the set uses detail::PageAlloc (THP, page release).
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
    using value_type = typename flat_hash_set::value_type;
    using difference_type = std::ptrdiff_t;
    using reference =
        const value_type&;
    using pointer =
        const value_type*;

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
    friend class flat_hash_set;
    template <bool>
    friend class iterator_impl;
    using map_ptr = typename std::conditional<IsConst, const flat_hash_set*,
                                              flat_hash_set*>::type;

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
    using value_type = typename flat_hash_set::value_type;
    using difference_type = std::ptrdiff_t;
    using reference =
        const value_type&;
    using pointer =
        const value_type*;

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
    friend class flat_hash_set;
    template <bool>
    friend class local_iterator_impl;
    using map_ptr = typename std::conditional<IsConst, const flat_hash_set*,
                                              flat_hash_set*>::type;

    local_iterator_impl(map_ptr m, size_t b, size_t i) noexcept
        : map_(m), bucket_(b), idx_(i) {
      if (map_) seek();
    }
    // Unified slot index, so the old table of a migration is covered too.
    void seek() noexcept {
      const size_t stop = map_->cap_ + map_->old_cap_;
      while (idx_ < stop &&
             !(detail::IsFull(map_->ctrl_at(idx_)) &&
               map_->bucket_index(*map_->slot_at(idx_)) == bucket_))
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
  flat_hash_set() noexcept(
      std::is_nothrow_default_constructible<Hash>::value &&
      std::is_nothrow_default_constructible<KeyEqual>::value &&
      std::is_nothrow_default_constructible<Allocator>::value) {}

  explicit flat_hash_set(size_type bucket_count, const Hash& hash = Hash(),
                         const KeyEqual& equal = KeyEqual(),
                         const Allocator& alloc = Allocator())
      : hash_(hash), equal_(equal), slot_alloc_(alloc), ctrl_alloc_(alloc) {
    if (bucket_count) rehash_to(min_cap_for(bucket_count));
  }

  flat_hash_set(size_type bucket_count, const Allocator& alloc)
      : flat_hash_set(bucket_count, Hash(), KeyEqual(), alloc) {}
  flat_hash_set(size_type bucket_count, const Hash& hash, const Allocator& alloc)
      : flat_hash_set(bucket_count, hash, KeyEqual(), alloc) {}

  explicit flat_hash_set(const Allocator& alloc)
      : hash_(), equal_(), slot_alloc_(alloc), ctrl_alloc_(alloc) {}

  template <typename InputIt>
  flat_hash_set(InputIt first, InputIt last,
                size_type bucket_count = 0, const Hash& hash = Hash(),
                const KeyEqual& equal = KeyEqual(),
                const Allocator& alloc = Allocator())
      : flat_hash_set(bucket_count, hash, equal, alloc) {
    insert(first, last);
  }

  template <typename InputIt>
  flat_hash_set(InputIt first, InputIt last, size_type bucket_count,
                const Allocator& alloc)
      : flat_hash_set(first, last, bucket_count, Hash(), KeyEqual(), alloc) {}

  template <typename InputIt>
  flat_hash_set(InputIt first, InputIt last, size_type bucket_count,
                const Hash& hash, const Allocator& alloc)
      : flat_hash_set(first, last, bucket_count, hash, KeyEqual(), alloc) {}

  flat_hash_set(std::initializer_list<value_type> il,
                size_type bucket_count = 0, const Hash& hash = Hash(),
                const KeyEqual& equal = KeyEqual(),
                const Allocator& alloc = Allocator())
      : flat_hash_set(il.begin(), il.end(), bucket_count, hash, equal, alloc) {}

  flat_hash_set(std::initializer_list<value_type> il, size_type bucket_count,
                const Allocator& alloc)
      : flat_hash_set(il, bucket_count, Hash(), KeyEqual(), alloc) {}

  flat_hash_set(std::initializer_list<value_type> il, size_type bucket_count,
                const Hash& hash, const Allocator& alloc)
      : flat_hash_set(il, bucket_count, hash, KeyEqual(), alloc) {}

  flat_hash_set(const flat_hash_set& other)
      : max_load_factor_(other.max_load_factor_),
        hash_(other.hash_),
        equal_(other.equal_),
        slot_alloc_(slot_traits::select_on_container_copy_construction(
            other.slot_alloc_)),
        ctrl_alloc_(ctrl_traits::select_on_container_copy_construction(
            other.ctrl_alloc_)) {
    copy_elements_from(other);
  }

  flat_hash_set(const flat_hash_set& other, const Allocator& alloc)
      : max_load_factor_(other.max_load_factor_),
        hash_(other.hash_),
        equal_(other.equal_),
        slot_alloc_(alloc),
        ctrl_alloc_(alloc) {
    copy_elements_from(other);
  }

  flat_hash_set(flat_hash_set&& other) noexcept
      : max_load_factor_(other.max_load_factor_),
        hash_(std::move(other.hash_)),
        equal_(std::move(other.equal_)),
        slot_alloc_(std::move(other.slot_alloc_)),
        ctrl_alloc_(std::move(other.ctrl_alloc_)) {
    steal_storage(other);
  }

  flat_hash_set(flat_hash_set&& other, const Allocator& alloc)
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

  ~flat_hash_set() {
    clear();
    free_storage();
  }

  flat_hash_set& operator=(const flat_hash_set& other) {
    if (this != &other) {
      flat_hash_set tmp(other);
      swap(tmp);
    }
    return *this;
  }

  flat_hash_set& operator=(flat_hash_set&& other) noexcept(
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

  flat_hash_set& operator=(std::initializer_list<value_type> il) {
    clear();
    insert(il);
    return *this;
  }

  void swap(flat_hash_set& other) noexcept(
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
    const uint64_t h = hash_mixed(v);
    const size_t dst = find_empty(h);
    construct_slot(dst, std::forward<V>(v));
    ctrl_[dst] = detail::H2(h);
  }

  // Precondition: *this is empty and has no storage.
  void copy_elements_from(const flat_hash_set& other) {
    if (other.size_ == 0) return;
    rehash_to(min_cap_for(other.size_));
    for_each_slot(other, [this](const value_type* s) { place_unique(*s); });
    size_ = other.size_;
    reset_growth_left();
  }

  // Precondition: *this is empty and has no storage.
  void move_elements_from(flat_hash_set& other) {
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
  void steal_storage(flat_hash_set& other) noexcept {
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
    other.overflow_ = empty_overflow();
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
    if (i < cap_) {
      if (const slot_type* s = first_full_in(ctrl_, slots_, cap_, i))
        return const_cast<value_type*>(
            reinterpret_cast<const value_type*>(static_cast<const void*>(s)));
      i = cap_;
    }
    if (const slot_type* s = first_full_in(old_ctrl_, old_slots_, old_cap_, i - cap_))
      return const_cast<value_type*>(
          reinterpret_cast<const value_type*>(static_cast<const void*>(s)));
    return nullptr;
  }

  // First full slot at index i or later of one table, or nullptr. Scans a
  // group at a time: a byte loop mispredicts on almost every slot when full
  // and free slots are mixed (e.g. after many erases).
  static const slot_type* first_full_in(const uint8_t* ctrl,
                                        const slot_type* slots, size_t cap,
                                        size_t i) noexcept {
    if (i >= cap) return nullptr;
    size_t g = i / kWidth * kWidth;
    Mask m = full_mask(Group::Load(ctrl + g)) &
             (Group::kAll << ((i - g) << Group::kShift));
    for (;;) {
      if (m) return &slots[g + Group::Lane(m)];
      g += kWidth;
      if (g >= cap) return nullptr;
      m = full_mask(Group::Load(ctrl + g));
    }
  }

  // Lanes of a group that hold an element.
  static Mask full_mask(const Group& g) noexcept {
    return ~(g.Match(detail::kEmpty) | g.Match(detail::kDeleted)) & Group::kAll;
  }

  static detail::Overflow* empty_overflow() noexcept {
    return const_cast<detail::Overflow*>(&detail::kEmptyOverflow);
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
                                   size_t cap, const detail::Overflow* overflow,
                                   const KeyEqual& equal) noexcept {
    const uint8_t hh = detail::H2(h);
    const size_t ng = cap / kWidth;
    const size_t home = detail::Fastrange(h, ng);
    {
      Group g = Group::Load(ctrl + home * kWidth);
      Mask m = g.Match(hh);
      while (m) {
        const value_type* e = reinterpret_cast<const value_type*>(
            &slots[home * kWidth + Group::Lane(m)]);
        if (equal(*e, key)) return e;
        m &= m - 1;
      }
      if (g.Match(detail::kEmpty)) return nullptr;
      if (!(overflow[home] & detail::OverflowBit(h))) return nullptr;
    }
    size_t gi = home + 1;
    for (size_t k = 1; k < ng; ++k) {
      if (gi >= ng) gi -= ng;
      Group g = Group::Load(ctrl + gi * kWidth);
      Mask m = g.Match(hh);
      while (m) {
        const unsigned j = Group::Lane(m);
        m &= m - 1;
        const value_type* e = reinterpret_cast<const value_type*>(
            &slots[gi * kWidth + j]);
        if (equal(*e, key)) return e;
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
      const Group g = Group::Load(old_ctrl_ + gi * kWidth);
      Mask m = g.Match(hh);
      while (m) {
        const value_type* e = reinterpret_cast<const value_type*>(
            &old_slots_[gi * kWidth + Group::Lane(m)]);
        if (equal_(*e, key)) return e;
        m &= m - 1;
      }
      if (g.Match(detail::kEmpty)) return nullptr;
      if (gi == home && !(old_overflow_[home] & detail::OverflowBit(h)))
        return nullptr;
    }
    return nullptr;
  }

  // Small trivially copyable keys go to the slow path by value, so callers
  // need not keep the key in memory for it.
  using slow_key_arg = typename std::conditional<
      std::is_trivially_copyable<key_type>::value &&
          sizeof(key_type) <= 2 * sizeof(void*),
      key_type, const key_type&>::type;

  // Element with key, or nullptr. Never modifies the set.
  //
  // The inlined part handles the common cases: a hit in the home group, and
  // a miss that the home group proves (a clear overflow bit), with
  // no migration. An empty slot in the home group would prove a miss too,
  // but testing it costs instructions on every miss; a set overflow bit is
  // rare enough that a probe of the next group is cheaper (inline or in
  // find_ptr_next(), see SFLAT_FIND_NEXT_INLINE). Everything else goes to
  // find_ptr_slow(). A small inlined part keeps callers' loops short: a
  // DRAM-bound find loop gets slower with every extra instruction (about
  // 0.4 ns each on Apple M2), since fewer finds then fit in the
  // out-of-order window.
  SFLAT_FIND_INLINE value_type* find_ptr(const key_type& key,
                                         uint64_t h) const noexcept {
    const size_t base = detail::HomeSlot(h, cap_);  // home group * kWidth
    const size_t home = base / kWidth;
#if SFLAT_PREFETCH_SLOTS
    {
      // Up to 256 bytes of the group (two 128-byte lines on Apple Silicon).
      // Integer arithmetic: slots_ is nullptr in an empty table.
      constexpr size_t kBytes = kWidth * sizeof(slot_type) < 256
                                    ? kWidth * sizeof(slot_type)
                                    : 256;
      const uintptr_t p =
           reinterpret_cast<uintptr_t>(slots_) + base * sizeof(slot_type);
      for (size_t o = 0; o < kBytes; o += 128)
        __builtin_prefetch(reinterpret_cast<const void*>(p + o));
    }
#endif
    const Group g = Group::Load(ctrl_ + base);
#if !SFLAT_LAZY_OVERFLOW
    // Read the overflow word now, not only after the control bytes say the
    // group is full (about a third of the groups at max load): the two loads
    // then overlap, and the miss test below has no hard-to-predict branch.
    const detail::Overflow ovf = overflow_[home] & detail::OverflowBit(h);
#endif
    Mask m = g.MatchH2(h);
    while (m) {
      const value_type* e = reinterpret_cast<const value_type*>(
          &slots_[base + Group::Lane(m)]);
      if (equal_(*e, key)) return const_cast<value_type*>(e);
      m &= m - 1;
    }
    if (SFLAT_LIKELY(old_cap_ == 0)) {
#if SFLAT_LAZY_OVERFLOW
      const detail::Overflow ovf = overflow_[home] & detail::OverflowBit(h);
#endif
      if (ovf == 0) return nullptr;
#if SFLAT_FIND_NEXT_INLINE
      // Keys that spill from the home group take the first group after it
      // with an empty slot. A slot never becomes empty again (erase leaves a
      // tombstone) until a rehash or migration rebuilds the table, so an
      // empty slot in the next group proves that the key is not further on.
      size_t nb = base + kWidth;
      if (nb == cap_) nb = 0;
      const Group g2 = Group::Load(ctrl_ + nb);
      Mask m2 = g2.MatchH2(h);
      while (m2) {
        const value_type* e = reinterpret_cast<const value_type*>(
            &slots_[nb + Group::Lane(m2)]);
        if (equal_(*e, key)) return const_cast<value_type*>(e);
        m2 &= m2 - 1;
      }
      if (g2.Match(detail::kEmpty)) return nullptr;
#else
      return find_ptr_next(key, h, base);
#endif
    }
    return find_ptr_slow(key, h);
  }

  // The probe past the home group when SFLAT_FIND_NEXT_INLINE is 0: a key
  // whose home group overflowed. Keys that spill from the home group take
  // the first group after it with an empty slot. A slot never becomes empty
  // again (erase leaves a tombstone) until a rehash or migration rebuilds the
  // table, so the first empty slot after the home group proves that the key
  // is not further on. Not migrating (old_cap_ == 0).
  // It probes on from the next group itself instead of handing over to
  // find_ptr_slow(), which would start again at the home group: full groups
  // come in runs, so the next group often has no empty slot (41 percent of
  // the hits and 81 percent of the misses that get here, i5-3230M at 10M).
#if defined(__GNUC__) || defined(__clang__)
  __attribute__((noinline, pure))
#endif
  value_type* find_ptr_next(slow_key_arg key, uint64_t h,
                            size_t base) const noexcept {
    size_t nb = base;
    for (size_t k = kWidth; k < cap_; k += kWidth) {
      nb += kWidth;
      if (nb == cap_) nb = 0;
      const Group g2 = Group::Load(ctrl_ + nb);
      Mask m2 = g2.MatchH2(h);
      while (m2) {
        const value_type* e = reinterpret_cast<const value_type*>(
            &slots_[nb + Group::Lane(m2)]);
        if (equal_(*e, key)) return const_cast<value_type*>(e);
        m2 &= m2 - 1;
      }
      if (g2.Match(detail::kEmpty)) return nullptr;
    }
    return nullptr;
  }

  // The rest of find_ptr(): probe past the home group, and the old table of
  // an ongoing migration. (Probes the home group again; it is in cache.)
  // `pure` (reads memory, writes none) lets callers keep the table fields in
  // registers across the call. It holds as long as equal_ has no side
  // effects, which the standard requires of KeyEqual.
#if defined(__GNUC__) || defined(__clang__)
  __attribute__((noinline, pure))
#endif
  value_type* find_ptr_slow(slow_key_arg key, uint64_t h) const noexcept {
    const value_type* p =
        find_in(key, h, ctrl_, slots_, cap_, overflow_, equal_);
    if (!p && old_cap_ != 0) p = find_in_old(key, h);
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
      Group g = Group::Load(ctrl_ + gi * kWidth);
      Mask m = g.Match(hh);
      while (m) {
        const unsigned j = Group::Lane(m);
        m &= m - 1;
        const size_t idx = gi * kWidth + j;
        if (equal_(*slot_at(idx), key)) return {idx, true};
      }
      if (track_del && first_del == cap_) {
        const Mask md = g.Match(detail::kDeleted);
        if (md) first_del = gi * kWidth + Group::Lane(md);
      }
      const Mask me = g.Match(detail::kEmpty);
      if (me) {
        size_t idx = gi * kWidth + Group::Lane(me);
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
      const Mask me =
          Group::Load(old_ctrl_ + gi * kWidth).Match(detail::kEmpty);
      if (me) {
        if (gi != home) old_overflow_[home] |= detail::OverflowBit(h);
        return gi * kWidth + Group::Lane(me);
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
  SFLAT_INSERT_INLINE inline std::pair<iterator, bool> insert_with(
      const key_type& key, Construct&& construct) {
    const uint64_t h = hash_mixed(key);
    if (SFLAT_LIKELY(growth_left_ != 0)) {
      // Fast path: no growth, no migration. Most keys are absent and their
      // home group has an empty slot. (An empty slot in the home group also
      // proves that the key is absent; tombstones do not matter here.)
      const size_t base = detail::HomeSlot(h, cap_);  // home group * kWidth
      const Group g = Group::Load(ctrl_ + base);
      Mask m = g.MatchH2(h);
      while (m) {
        value_type* e = cur_slot(base + Group::Lane(m));
        if (equal_(*e, key)) return {iterator(this, e), false};
        m &= m - 1;
      }
      const Mask me = g.Match(detail::kEmpty);
      if (me) {
        const size_t idx = base + Group::Lane(me);
        value_type* e = cur_slot(idx);
        construct(static_cast<void*>(e));
        ctrl_[idx] = detail::H2(h);
        ++size_;
        --growth_left_;
        return {iterator(this, e), true};
      }
    }
    return insert_with_slow(key, h, construct);
  }

  // The rest of insert_with(): the key's home group is full (and the key is
  // not in it), or the table must grow or is migrating.
  template <typename Construct>
  SFLAT_NOINLINE std::pair<iterator, bool> insert_with_slow(
      const key_type& key, uint64_t h, Construct&& construct) {
    if (growth_left_ != 0) {
      if (deleted_ == 0) {
        // No tombstones, so the key goes to the first empty slot after the
        // home group. (With tombstones, find_or_prepare() below reuses the
        // first one.) growth_left_ != 0 means there is no migration and at
        // least one empty slot, so the loop ends. If the home group's
        // overflow bit is clear, no key with this bit spilled from it, so
        // the later groups cannot hold the key and only empty slots matter.
        const size_t home = detail::HomeSlot(h, cap_) / kWidth;
        const size_t ng = cap_ / kWidth;
        const detail::Overflow bit = detail::OverflowBit(h);
        const bool may_spill = (overflow_[home] & bit) != 0;
        size_t gi = home;
        for (;;) {
          if (++gi == ng) gi = 0;
          const Group g = Group::Load(ctrl_ + gi * kWidth);
          if (may_spill) {
            Mask m = g.MatchH2(h);
            while(m) {
              value_type* e = cur_slot(gi * kWidth + Group::Lane(m));
              if (equal_(*e, key)) return {iterator(this, e), false};
              m &= m - 1;
            }
          }
          const Mask me = g.Match(detail::kEmpty);
          if (me) {
            const size_t idx = gi * kWidth + Group::Lane(me);
            value_type* e = cur_slot(idx);
            construct(static_cast<void*>(e));
            ctrl_[idx] = detail::H2(h);
            overflow_[home] |= bit;
            ++size_;
            --growth_left_;
            return {iterator(this, e), true};
          }
        }
      }
    } else {
      return insert_with_growth(key, h, construct);
    }
    const Probe p = locate_for_insert(key, h);
    if (!p.found) {
      construct(static_cast<void*>(slot_at(p.idx)));
      commit_insert(p.idx, h);
      reset_growth_left();
    }
    return {iterator(this, slot_at(p.idx)), !p.found};
  }

  // Insert that first grows, migrates or rehashes the table. That moves
  // elements and may free the old table, while key and the arguments captured
  // by construct may refer to an element (s.insert(*it), s.emplace(*it)).
  // So use them before the table changes: look
  // the key up, then build the value in a temporary, then prepare the table.
  // Costs one more lookup and move, only on this path. As a bonus the table is
  // untouched if construction throws.
  template <typename Construct>
  SFLAT_NOINLINE std::pair<iterator, bool> insert_with_growth(
      const key_type& key, uint64_t h, Construct& construct) {
    if (value_type* e = find_ptr(key, h)) return {iterator(this, e), false};
    alignas(value_type) unsigned char buf[sizeof(value_type)];
    construct(static_cast<void*>(buf));
    struct Temp {
      value_type* v;
      ~Temp() { v->~value_type(); }
    } tmp{std::launder(reinterpret_cast<value_type*>(buf))};
    prepare_insert_slow();
    const Probe p = locate_for_insert(*tmp.v, h);
    assert(!p.found);
    ::new (static_cast<void*>(slot_at(p.idx))) value_type(std::move(*tmp.v));
    commit_insert(p.idx, h);
    reset_growth_left();
    return {iterator(this, slot_at(p.idx)), true};
  }

  // First empty slot for a hash (table is all-empty on entry paths that use
  // this, e.g. rehash/copy).
  size_t find_empty(uint64_t h) noexcept {
    const size_t ng = cap_ / kWidth;
    const size_t home = detail::Fastrange(h, ng);
    size_t gi = home;
    for (size_t k = 0; k < ng; ++k) {
      if (gi >= ng) gi -= ng;
      Group g = Group::Load(ctrl_ + gi * kWidth);
      const Mask me = g.Match(detail::kEmpty);
      if (me) {
        const size_t dst = gi * kWidth + Group::Lane(me);
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
      throw std::length_error("sflat::flat_hash_set: too many elements");
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
  // Overflow words for ng groups, all clear. A user allocator is rebound to
  // the word type, so the words are aligned (a byte allocation need not be).
  using ovf_alloc = typename std::allocator_traits<Allocator>::template rebind_alloc<
      detail::Overflow>;
  using ovf_traits = std::allocator_traits<ovf_alloc>;
  detail::Overflow* alloc_overflow(size_t ng) {
    const size_t bytes = ng * sizeof(detail::Overflow);
    detail::Overflow* p;
    if constexpr (kOwnMemory) {
      p = static_cast<detail::Overflow*>(
          detail::PageAlloc(bytes, detail::kCacheLine, true));
    } else {
      ovf_alloc a(ctrl_alloc_);
      p = ovf_traits::allocate(a, ng);
    }
    std::memset(p, 0, bytes);
    return p;
  }
  void free_overflow(detail::Overflow* p, size_t ng) noexcept {
    if constexpr (kOwnMemory) {
      detail::PageFree(p, ng * sizeof(detail::Overflow), detail::kCacheLine);
    } else if (p) {
      ovf_alloc a(ctrl_alloc_);
      ovf_traits::deallocate(a, p, ng);
    }
  }
  void free_bytes(uint8_t* p, size_t n) noexcept {
    if constexpr (kOwnMemory)
      detail::PageFree(p, n, detail::kCacheLine);
    else if (p)
      ctrl_traits::deallocate(ctrl_alloc_, p, n);
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
    detail::Overflow* new_overflow = alloc_overflow(new_ng);
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
    const uint64_t h = hash_mixed(*s);
    // Fast path: an empty slot in the home group (the new table is at most
    // ~60% full during a migration).
    const size_t home = detail::Fastrange(h, cap_ / kWidth);
    const Mask me =
        Group::Load(ctrl_ + home * kWidth).Match(detail::kEmpty);
    const size_t dst = me ? home * kWidth + Group::Lane(me) : find_empty(h);
    ::new (static_cast<void*>(&slots_[dst])) value_type(std::move(*s));
    s->~value_type();
    ctrl_[dst] = detail::H2(h);
  }

  // Migrate up to `batch` old slots (a multiple of kWidth), one group at a
  // time, then manage pages: release old slots behind the front, and ask for
  // huge pages in the new table up to just ahead of the front.
  void migrate_step(size_t batch = kMigrateBatch) {
    if (!is_migrating()) return;
    const size_t stop = old_cap_ - mig_pos_ > batch ? mig_pos_ + batch : old_cap_;
    for (; mig_pos_ < stop; mig_pos_ += kWidth) {
      uint8_t* oc = old_ctrl_ + mig_pos_;
      const Group g = Group::Load(oc);
      Mask full =
          ~(g.Match(detail::kEmpty) | g.Match(detail::kDeleted)) & Group::kAll;
      while (full) {
        migrate_one(mig_pos_ + Group::Lane(full));
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
    detail::Overflow* new_overflow = empty_overflow();
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
          const uint64_t h = hash_mixed(*s);
          // locate empty slot in the fresh table
          const size_t home = detail::Fastrange(h, new_ng);
          size_t gi = home;
          size_t dst = new_cap;
          for (size_t k = 0; k < new_ng; ++k) {
            if (gi >= new_ng) gi -= new_ng;
            Group g = Group::Load(new_ctrl + gi * kWidth);
            const Mask me = g.Match(detail::kEmpty);
            if (me) {
              dst = gi * kWidth + Group::Lane(me);
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
    // Leave a valid empty table: no pointer to the freed arrays survives,
    // and growth_left_ == 0 sends the next insert to the slow path.
    slots_ = nullptr;
    ctrl_ = empty_ctrl();
    overflow_ = empty_overflow();
    cap_ = 0;
    deleted_ = 0;
    limit_ = 0;
    growth_left_ = 0;
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
      std::memset(overflow_, 0, cap_ / kWidth * sizeof(detail::Overflow));
    }
    size_ = 0;
    deleted_ = 0;
    reset_growth_left();
  }

  std::pair<iterator, bool> insert(const value_type& v) {
    return insert_with(v,
                       [&](void* p) { ::new (p) value_type(v); });
  }

  std::pair<iterator, bool> insert(value_type&& v) {
    return insert_with(v,
                       [&](void* p) { ::new (p) value_type(std::move(v)); });
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

  // emplace(key) with an exact key_type: inserts it directly instead of
  // through a temporary (the generic emplace below).
  template <typename K,
            typename std::enable_if<
                std::is_same<typename std::decay<K>::type, key_type>::value,
                int>::type = 0>
  std::pair<iterator, bool> emplace(K&& k) {
    return insert(std::forward<K>(k));
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

  // Same as erase(find(key)), but without the scan for the next full slot
  // that erase(const_iterator) does to build its return value. That scan
  // reads memory the caller never uses: on Apple M2 erase by key got 17
  // percent faster at 10M entries and 18 percent at 100M.
  size_type erase(const key_type& key) {
    value_type* const e = find_ptr(key, hash_mixed(key));
    if (!e) return 0;
    const size_t i = index_of(e);
    destroy_slot(i);
    if (i < cap_) {
      ctrl_[i] = detail::kDeleted;
      ++deleted_;
    } else {
      old_ctrl_[i - cap_] = detail::kDeleted;  // the migration skips it
    }
    --size_;
    return 1;
  }

  // ------------------------------------------------------------------ lookup
  // find() never changes the set, also during a migration. Concurrent finds
  // from many threads are safe while no thread writes.
  SFLAT_FIND_INLINE iterator find(const key_type& key) {
    return iterator(this, find_ptr(key, hash_mixed(key)));
  }

  SFLAT_FIND_INLINE const_iterator find(const key_type& key) const {
    return const_iterator(this, find_ptr(key, hash_mixed(key)));
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

  // The elements with bucket(key) == n, as the standard defines it (the
  // length of [begin(n), end(n))). Bucket n is not slot n: an element is
  // stored near, not at, its bucket. Linear in the table size, like the
  // local iterators.
  size_type bucket_size(size_type n) const {
    assert(n < cap_);
    return static_cast<size_type>(std::distance(begin(n), end(n)));
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
  local_iterator end(size_type n) {
    return local_iterator(this, n, cap_ + old_cap_);
  }
  const_local_iterator end(size_type n) const {
    return const_local_iterator(this, n, cap_ + old_cap_);
  }
  const_local_iterator cend(size_type n) const {
    return const_local_iterator(this, n, cap_ + old_cap_);
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
  detail::Overflow* overflow_ = empty_overflow();  // size cap_/kWidth; per-group overflow Bloom bits
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
  detail::Overflow* old_overflow_ = nullptr;
  size_type old_cap_ = 0;
  size_type mig_pos_ = 0;       // next old slot to migrate
  size_type old_released_ = 0;  // old slot bytes given back to the kernel
  size_type huge_hint_ = 0;     // new slot bytes with the huge page hint
};

template <typename Key, typename Hash, typename KeyEqual, typename Allocator>
bool operator==(const flat_hash_set<Key, Hash, KeyEqual, Allocator>& a,
                const flat_hash_set<Key, Hash, KeyEqual, Allocator>& b) {
  if (a.size() != b.size()) return false;
  for (const auto& k : a)
    if (b.find(k) == b.end()) return false;
  return true;
}

template <typename Key, typename Hash, typename KeyEqual, typename Allocator>
bool operator!=(const flat_hash_set<Key, Hash, KeyEqual, Allocator>& a,
                const flat_hash_set<Key, Hash, KeyEqual, Allocator>& b) {
  return !(a == b);
}

template <typename Key, typename Hash, typename KeyEqual, typename Allocator>
void swap(flat_hash_set<Key, Hash, KeyEqual, Allocator>& a,
          flat_hash_set<Key, Hash, KeyEqual, Allocator>& b) noexcept(
      noexcept(a.swap(b))) {
  a.swap(b);
}

}  // namespace sflat
