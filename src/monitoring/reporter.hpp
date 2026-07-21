#pragma once

#include <cstdint>
#include <cstdio>

#include "monitoring/histogram.hpp"

namespace lab {

// Cold-path snapshot printer. The hot path only touches LatencyHistogram;
// this formats a periodic report from copies handed over by the pipeline.
struct StageStats {
  const char* name;
  const LatencyHistogram* hist;
};

inline void print_report(FILE* out, uint64_t elapsed_ns, uint64_t msgs,
                         uint64_t trades, const StageStats* stages,
                         int n_stages) {
  const double secs = static_cast<double>(elapsed_ns) / 1e9;
  fprintf(out, "\n=== latency-lab report (%.1fs) ===\n", secs);
  fprintf(out, "messages: %lu (%.0f msg/s)   trades: %lu\n",
          static_cast<unsigned long>(msgs),
          secs > 0 ? static_cast<double>(msgs) / secs : 0.0,
          static_cast<unsigned long>(trades));
  fprintf(out, "%-22s %10s %10s %10s %10s %10s %10s\n", "stage (ns)", "count",
          "mean", "p50", "p99", "p99.9", "max");
  for (int i = 0; i < n_stages; ++i) {
    const LatencyHistogram& h = *stages[i].hist;
    fprintf(out, "%-22s %10lu %10lu %10lu %10lu %10lu %10lu\n",
            stages[i].name, static_cast<unsigned long>(h.count()),
            static_cast<unsigned long>(h.mean()),
            static_cast<unsigned long>(h.percentile(50)),
            static_cast<unsigned long>(h.percentile(99)),
            static_cast<unsigned long>(h.percentile(99.9)),
            static_cast<unsigned long>(h.max()));
  }
  fflush(out);
}

}  // namespace lab
