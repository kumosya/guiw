#include "process.h"

#include <cstring>

#include "os/core_service.h"
namespace taskmgr {
bool sample(Snapshot& next, const Snapshot& previous) {
    next.count = 0;
    next.error = 0;
    next.truncated = false;
    next.timestamp = os::Clock::getMonotonicTime();
    uint64_t cursor = 0;
    while (next.count < capacity) {
        os::IpcMessage request{}, reply{};
        request.type = uint64_t(os::TaskOperation::Enumerate);
        request.length = 4;
        request.data[0] = cursor;
        request.data[1] = 1;
        const int status =
            os::CoreServiceClient::result(os::CoreServiceKind::Task, request, reply);
        if (status) {
            if (status != int(os::ErrorCode::NotFound)) next.error = status;
            break;
        }
        auto& row = next.rows[next.count];
        row = {};
        memcpy(&row.info, &reply.data[1], sizeof(row.info));
        row.info.name[sizeof(row.info.name) - 1] = 0;
        row.info.commandLine[sizeof(row.info.commandLine) - 1] = 0;
        const uint32_t commandLength =
            row.info.commandLineSize < sizeof(row.info.commandLine)
                ? row.info.commandLineSize
                : sizeof(row.info.commandLine) - 1;
        for (uint32_t i = 0; i + 1 < commandLength; ++i)
            if (!row.info.commandLine[i]) row.info.commandLine[i] = ' ';
        if (row.info.id < cursor || row.info.id == UINT64_MAX) {
            next.error = int(os::ErrorCode::InvalidArgument);
            break;
        }
        cursor = row.info.id + 1;
        for (uint32_t i = 0; i < previous.count; ++i) {
            const auto& old = previous.rows[i];
            if (old.info.id == row.info.id &&
                old.info.startedNanoseconds == row.info.startedNanoseconds &&
                row.info.cpuNanoseconds >= old.info.cpuNanoseconds &&
                next.timestamp > previous.timestamp) {
                const __uint128_t usage =
                    __uint128_t(row.info.cpuNanoseconds - old.info.cpuNanoseconds) *
                    10000 / (next.timestamp - previous.timestamp);
                row.cpu = usage > UINT32_MAX ? UINT32_MAX : uint32_t(usage);
                row.sampled = true;
                break;
            }
        }
        ++next.count;
    }
    next.truncated = next.count == capacity;
    return !next.error;
}
int terminate(const os::TaskInfo& identity, bool force) {
    os::TaskInfo current{};
    int status = os::Task::query(identity.id, current);
    if (status) return status;
    if (current.startedNanoseconds != identity.startedNanoseconds ||
        current.id != identity.id)
        return int(os::ErrorCode::NotFound);
    // OS task IDs monotonically increase (64-bit nextTaskId); slots are reused,
    // IDs are not. Revalidate the start time as an additional identity guard.
    // The kernel enforces user permissions and managed-service protection.
    return os::Task::sendSignal(identity.id, force ? 9 : 15);
}
}  // namespace taskmgr
