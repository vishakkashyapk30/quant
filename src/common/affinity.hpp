#pragma once

#include <pthread.h>
#include <sched.h>

namespace lab {

// Pins the calling thread to a single CPU. Returns false on failure (e.g.
// cpu out of range). Pinning keeps the hot path's working set in one core's
// L1/L2 and avoids scheduler migrations that blow the cache.
inline bool pin_this_thread(int cpu) {
  if (cpu < 0) return false;
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(static_cast<unsigned>(cpu), &set);
  return pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0;
}

}  // namespace lab
