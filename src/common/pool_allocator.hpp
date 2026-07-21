#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "common/huge_alloc.hpp"

namespace lab {

// Fixed-size object pool with an intrusive free list.
//
// allocate/free are O(1) pointer swaps; memory is acquired in large slabs
// up front so the hot path never calls new/delete. If the pool is exhausted
// it grows by another slab (cold path) and counts the growth so tests /
// benchmarks can assert "zero allocations after warmup".
//
// Slabs can optionally be huge-page backed (see huge_alloc.hpp): a large
// pool on 2 MiB pages needs a handful of TLB entries instead of hundreds,
// which matters because pool access from the order book is random-access.
template <typename T>
class ObjectPool {
  union Node {
    Node* next;
    alignas(T) unsigned char storage[sizeof(T)];
  };
  static_assert(offsetof(Node, storage) == 0);

 public:
  explicit ObjectPool(size_t slab_size, bool huge_pages = false)
      : slab_size_(slab_size), huge_pages_(huge_pages) {
    add_slab();
  }

  ~ObjectPool() {
    for (const SlabMem& s : slabs_) free_slab(s);
  }

  ObjectPool(const ObjectPool&) = delete;
  ObjectPool& operator=(const ObjectPool&) = delete;

  template <typename... Args>
  T* construct(Args&&... args) {
    if (free_ == nullptr) {
      add_slab();  // cold path
    }
    Node* n = free_;
    free_ = n->next;
    ++live_;
    if (live_ > high_water_) high_water_ = live_;
    return ::new (static_cast<void*>(n->storage)) T(std::forward<Args>(args)...);
  }

  void destroy(T* p) {
    p->~T();
    Node* n = reinterpret_cast<Node*>(p);
    n->next = free_;
    free_ = n;
    --live_;
  }

  size_t live() const { return live_; }
  size_t high_water() const { return high_water_; }
  size_t slab_count() const { return slabs_.size(); }
  size_t capacity() const { return slabs_.size() * slab_size_; }
  PageBacking backing() const {
    return slabs_.empty() ? PageBacking::kNormal : slabs_.front().backing;
  }

 private:
  void add_slab() {
    SlabMem mem = alloc_slab(slab_size_ * sizeof(Node), huge_pages_);
    slabs_.push_back(mem);
    Node* slab = static_cast<Node*>(mem.ptr);
    for (size_t i = 0; i < slab_size_; ++i) {
      slab[i].next = free_;
      free_ = &slab[i];
    }
  }

  std::vector<SlabMem> slabs_;
  Node* free_ = nullptr;
  size_t slab_size_;
  bool huge_pages_;
  size_t live_ = 0;
  size_t high_water_ = 0;
};

}  // namespace lab
