#pragma once
#include "os/abi.h"
#include "osgui/protocol.h"
namespace osgui {
inline bool setSessionActive(uint64_t service, bool active) {
    os::IpcMessage request{}, reply{};
    request.type = static_cast<uint64_t>(WindowRequest::SetSessionActive);
    request.data[0] = active ? 1 : 0;
    request.length = 1;
    return os::callService(service, request, reply) == 0 && reply.data[0] == 0;
}
}  // namespace osgui
