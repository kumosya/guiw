#pragma once
#include "os/task.h"
namespace taskmgr {
constexpr uint32_t capacity = 256;
struct Process {
    os::TaskInfo info{};
    uint32_t cpu = 0;
    bool sampled = false;
};
struct Snapshot {
    Process rows[capacity];
    uint32_t count = 0;
    uint64_t timestamp = 0;
    bool truncated = false;
    int error = 0;
};
bool sample(Snapshot& next, const Snapshot& previous);
int terminate(const os::TaskInfo& identity, bool force);
}  // namespace taskmgr
