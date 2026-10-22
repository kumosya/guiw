#pragma once
#include "osgui/font.h"
#include "osgui/protocol.h"
namespace osgui::detail {
inline bool validBatch(const DrawBatch& batch) {
    if (batch.version != protocolVersion || batch.count > batchCommandCapacity ||
        batch.textLength > batchTextCapacity || batch.present > 1)
        return false;
    uint64_t work = 0;
    for (uint32_t i = 0; i < batch.count; ++i) {
        const auto& command = batch.commands[i];
        if (command.x < -4096 || command.x > 4096 || command.y < -4096 ||
            command.y > 4096)
            return false;
        if (command.kind == DrawKind::Fill) {
            if (command.width > 704 || command.height > 448) return false;
            work += uint64_t(command.width) * command.height;
        } else if (command.kind == DrawKind::Text) {
            if (command.fontFace > 2 || command.fontSize < 8 || command.fontSize > 48 ||
                command.offset > batch.textLength ||
                command.length > batch.textLength - command.offset ||
                !utf8::valid(batch.text + command.offset, command.length))
                return false;
            work += uint64_t(command.length) * command.fontSize * command.fontSize;
        } else
            return false;
        if (work > 2 * 1024 * 1024) return false;
    }
    return true;
}
}  // namespace osgui::detail
