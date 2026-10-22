#pragma once
#include <cstdio>

#include "os/abi.h"
#include "os/task.h"
namespace osgui::detail {
class Profile {
    static constexpr unsigned capacity = 512;
    uint64_t frames_[capacity]{}, latencies_[capacity]{};
    unsigned frames = 0, latencies = 0;
    uint64_t wakeups = 0, ipc = 0, area = 0, bytes = 0, lastReport = 0, lastCpu = 0;
    static uint64_t percentile(const uint64_t* values, unsigned count,
                               unsigned percentile) {
        if (!count) return 0;
        uint64_t sorted[capacity];
        for (unsigned i = 0; i < count; ++i) {
            unsigned j = i;
            while (j && sorted[j - 1] > values[i]) {
                sorted[j] = sorted[j - 1];
                --j;
            }
            sorted[j] = values[i];
        }
        return sorted[(uint64_t(count - 1) * percentile + 99) / 100];
    }

   public:
    void request() { __atomic_add_fetch(&ipc, 1, __ATOMIC_RELAXED); }
    void wake() { ++wakeups; }
    void frame(uint64_t elapsed, uint64_t latency, uint64_t calls, uint64_t pixels,
               uint64_t outputBytes) {
        frames_[frames % capacity] = elapsed;
        ++frames;
        if (latency) {
            latencies_[latencies % capacity] = latency;
            ++latencies;
        }
        (void)calls;
        area += pixels;
        bytes += outputBytes;
    }
    void report(uint64_t now, uint64_t surfacePeak) {
        if (!lastReport) {
            lastReport = now;
            os::TaskInfo self{};
            if (!os::Task::query(0, self)) lastCpu = self.cpuNanoseconds;
            return;
        }
        if (now - lastReport < 5000000000ULL) return;
        os::TaskInfo self{};
        const bool cpuValid =
            !os::Task::query(0, self) && self.cpuNanoseconds >= lastCpu;
        uint64_t cpu = cpuValid ? uint64_t(__uint128_t(self.cpuNanoseconds - lastCpu) *
                                           1000 / (now - lastReport))
                                : 0;
        const unsigned count = frames < capacity ? frames : capacity,
                       inputs = latencies < capacity ? latencies : capacity;
        char line[512];
        const auto calls = __atomic_exchange_n(&ipc, 0, __ATOMIC_RELAXED);
        snprintf(line, sizeof(line),
                 "[GUIW-PROFILE] frames=%u samples=%u frame_ns_p50=%llu p95=%llu "
                 "p99=%llu input_samples=%u input_ns_p95=%llu cpu_permille=%llu "
                 "cpu_valid=%u ipc=%llu damage_pixels=%llu output_bytes=%llu "
                 "wakes=%llu surface_peak_bytes=%llu\n",
                 frames, count, (unsigned long long)percentile(frames_, count, 50),
                 (unsigned long long)percentile(frames_, count, 95),
                 (unsigned long long)percentile(frames_, count, 99), inputs,
                 (unsigned long long)percentile(latencies_, inputs, 95),
                 (unsigned long long)cpu, cpuValid ? 1 : 0, (unsigned long long)calls,
                 (unsigned long long)area, (unsigned long long)bytes,
                 (unsigned long long)wakeups, (unsigned long long)surfacePeak);
        os::System::log(line);
        lastReport = now;
        lastCpu = self.cpuNanoseconds;
        frames = latencies = 0;
        wakeups = area = bytes = 0;
    }
};
}  // namespace osgui::detail
