#pragma once

#include <sys/mman.h>

#include <cstddef>
#include <new>

namespace lab {

// Slab memory with optional huge-page backing.
//
// Fallback chain when huge pages are requested:
//   1. mmap(MAP_HUGETLB)          - explicit 2 MiB pages; needs vm.nr_hugepages
//   2. mmap + madvise(HUGEPAGE)   - transparent huge pages (works unprivileged
//                                   when THP is in `madvise` or `always` mode)
//   3. plain mmap                 - regular 4 KiB pages
//
// Why bother: a multi-MiB order pool on 4 KiB pages occupies hundreds of TLB
// entries; on 2 MiB pages it needs a handful, removing dTLB misses from the
// book's random-access hot path.
enum class PageBacking : unsigned char { kNormal, kTransparentHuge, kExplicitHuge };

struct SlabMem {
  void* ptr = nullptr;
  size_t bytes = 0;
  PageBacking backing = PageBacking::kNormal;
};

inline constexpr size_t kHugePageBytes = 2 * 1024 * 1024;

inline SlabMem alloc_slab(size_t bytes, bool try_huge) {
  if (try_huge) {
    const size_t rounded = (bytes + kHugePageBytes - 1) & ~(kHugePageBytes - 1);
    void* p = ::mmap(nullptr, rounded, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
    if (p != MAP_FAILED) {
      return {p, rounded, PageBacking::kExplicitHuge};
    }
    p = ::mmap(nullptr, rounded, PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p != MAP_FAILED) {
      ::madvise(p, rounded, MADV_HUGEPAGE);  // best effort
      return {p, rounded, PageBacking::kTransparentHuge};
    }
  }
  void* p = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED) throw std::bad_alloc{};
  return {p, bytes, PageBacking::kNormal};
}

inline void free_slab(const SlabMem& m) {
  if (m.ptr != nullptr) ::munmap(m.ptr, m.bytes);
}

inline const char* backing_name(PageBacking b) {
  switch (b) {
    case PageBacking::kExplicitHuge: return "hugetlb";
    case PageBacking::kTransparentHuge: return "thp";
    default: return "normal";
  }
}

}  // namespace lab
