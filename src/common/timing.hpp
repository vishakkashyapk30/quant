#pragma once

#include <cstdint>
#include <ctime>

#if defined(__x86_64__)
#include <x86intrin.h>
#endif

namespace lab {

inline uint64_t now_ns() {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ull +
         static_cast<uint64_t>(ts.tv_nsec);
}

#if defined(__x86_64__)
inline uint64_t rdtsc() { return __rdtsc(); }

// Serializing variant: prevents the CPU from reordering the timestamp read
// past earlier instructions. Use at measurement boundaries.
inline uint64_t rdtscp() {
  unsigned aux;
  return __rdtscp(&aux);
}

inline void cpu_pause() { _mm_pause(); }
#else
inline void cpu_pause() {}
#endif

// Calibrates TSC frequency against CLOCK_MONOTONIC so raw cycle counts can be
// converted to nanoseconds. Calibration is a one-off cold-path cost.
class TscClock {
 public:
  static double ticks_per_ns() {
    static const double v = calibrate();
    return v;
  }

  static uint64_t to_ns(uint64_t ticks) {
    return static_cast<uint64_t>(static_cast<double>(ticks) / ticks_per_ns());
  }

 private:
  static double calibrate() {
#if defined(__x86_64__)
    const uint64_t t0_ns = now_ns();
    const uint64_t t0 = rdtscp();
    timespec req{0, 20'000'000};  // 20 ms
    nanosleep(&req, nullptr);
    const uint64_t t1 = rdtscp();
    const uint64_t t1_ns = now_ns();
    return static_cast<double>(t1 - t0) / static_cast<double>(t1_ns - t0_ns);
#else
    return 1.0;
#endif
  }
};

}  // namespace lab
