#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace lab {

// Open-addressing hash map from uint64_t keys to trivially-copyable values,
// using linear probing with backward-shift deletion (no tombstones).
//
// Rationale vs std::unordered_map: one flat contiguous array, no per-node
// allocation, no pointer chasing — lookups touch one or two cache lines.
// Key 0 is reserved as the empty sentinel (order ids start at 1).
template <typename V>
class FlatHashMap {
  struct Slot {
    uint64_t key;
    V value;
  };
  static constexpr uint64_t kEmpty = 0;

 public:
  explicit FlatHashMap(size_t initial_capacity = 1024)
      : slots_(round_up_pow2(initial_capacity)),
        mask_(slots_.size() - 1) {}

  V* find(uint64_t key) {
    assert(key != kEmpty);
    size_t i = index_of(key);
    while (slots_[i].key != kEmpty) {
      if (slots_[i].key == key) return &slots_[i].value;
      i = (i + 1) & mask_;
    }
    return nullptr;
  }

  // Returns false (and leaves the map unchanged) if the key already exists.
  bool insert(uint64_t key, const V& value) {
    assert(key != kEmpty);
    if ((size_ + 1) * 10 > slots_.size() * 7) grow();
    size_t i = index_of(key);
    while (slots_[i].key != kEmpty) {
      if (slots_[i].key == key) return false;
      i = (i + 1) & mask_;
    }
    slots_[i] = Slot{key, value};
    ++size_;
    return true;
  }

  bool erase(uint64_t key) {
    assert(key != kEmpty);
    size_t i = index_of(key);
    while (slots_[i].key != key) {
      if (slots_[i].key == kEmpty) return false;
      i = (i + 1) & mask_;
    }
    // Backward-shift: pull later probe-chain members into the hole so probe
    // sequences stay contiguous without tombstones.
    size_t hole = i;
    size_t j = i;
    while (true) {
      j = (j + 1) & mask_;
      if (slots_[j].key == kEmpty) break;
      const size_t home = index_of(slots_[j].key);
      if (((j - home) & mask_) >= ((j - hole) & mask_)) {
        slots_[hole] = slots_[j];
        hole = j;
      }
    }
    slots_[hole].key = kEmpty;
    --size_;
    return true;
  }

  size_t size() const { return size_; }
  size_t capacity() const { return slots_.size(); }

 private:
  static size_t round_up_pow2(size_t v) {
    size_t p = 16;
    while (p < v) p <<= 1;
    return p;
  }

  // splitmix64 finalizer: cheap, well-distributed for sequential ids.
  static uint64_t mix(uint64_t x) {
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ull;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebull;
    x ^= x >> 31;
    return x;
  }

  size_t index_of(uint64_t key) const { return mix(key) & mask_; }

  void grow() {
    std::vector<Slot> old = std::move(slots_);
    slots_.assign(old.size() * 2, Slot{kEmpty, V{}});
    mask_ = slots_.size() - 1;
    size_ = 0;
    for (const Slot& s : old) {
      if (s.key != kEmpty) insert(s.key, s.value);
    }
  }

  std::vector<Slot> slots_;
  size_t mask_;
  size_t size_ = 0;
};

}  // namespace lab
