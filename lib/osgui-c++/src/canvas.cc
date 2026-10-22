#include <cstring>

#include "os/abi.h"
#include "osgui/gui.h"
namespace osgui {
namespace {
bool supports(uint64_t service, uint64_t capability) {
    os::IpcMessage request{}, reply{};
    request.type = uint64_t(WindowRequest::Negotiate);
    request.data[0] = protocolVersion;
    request.length = 1;
    return !os::callService(service, request, reply) && reply.length >= 3 &&
           !reply.data[0] && reply.data[1] == protocolVersion &&
           (reply.data[2] & capability) == capability;
}
}  // namespace
Canvas::Canvas(uint64_t service, WindowHandle window)
    : service_(service), window_(window), good_(supports(service, capabilityBatch)) {
    batch_.version = protocolVersion;
}
bool Canvas::flush(bool present) {
    if (!good_) return false;
    os::IpcMessage request{}, reply{};
    static_assert(sizeof(batch_) + 8 <= sizeof(request.data));
    request.type = uint64_t(WindowRequest::DrawBatch);
    request.data[0] = window_;
    batch_.present = present ? 1 : 0;
    memcpy(&request.data[1], &batch_, sizeof(batch_));
    request.length = 1 + (sizeof(batch_) + 7) / 8;
    good_ = !os::callService(service_, request, reply) && !reply.data[0];
    batch_.count = 0;
    batch_.textLength = 0;
    batch_.present = 0;
    return good_;
}
bool Canvas::fill(int32_t x, int32_t y, uint32_t width, uint32_t height,
                  uint32_t color) {
    if (batch_.count == batchCommandCapacity && !flush()) return false;
    if (!good_ || width > 704 || height > 448) return false;
    batch_.commands[batch_.count++] = {DrawKind::Fill, x, y, width, height,
                                       color,          0, 0, 0,     0};
    return true;
}
bool Canvas::text(int32_t x, int32_t y, const char* text, uint32_t color, FontFace face,
                  uint32_t size) {
    const size_t length = text ? strlen(text) : 0;
    if (length > batchTextCapacity || !utf8::valid(text, length)) return false;
    if ((batch_.count == batchCommandCapacity ||
         batch_.textLength + length > batchTextCapacity) &&
        !flush())
        return false;
    if (!good_) return false;
    batch_.commands[batch_.count++] = {
        DrawKind::Text, x,   y, 0, 0, color, batch_.textLength, uint32_t(length),
        uint32_t(face), size};
    memcpy(batch_.text + batch_.textLength, text, length);
    batch_.textLength += length;
    return true;
}
bool Clipboard::set(uint64_t service, const char* text, size_t length) {
    if (length > clipboardCapacity || !utf8::valid(text, length) ||
        !supports(service, capabilityClipboard))
        return false;
    os::IpcMessage request{}, reply{};
    request.type = uint64_t(WindowRequest::ClipboardSet);
    request.data[0] = protocolVersion;
    request.data[1] = length;
    memcpy(&request.data[2], text, length);
    request.length = 2 + (length + 7) / 8;
    return !os::callService(service, request, reply) && !reply.data[0];
}
bool Clipboard::get(uint64_t service, char* text, size_t capacity, size_t& length) {
    length = 0;
    if (!text || !supports(service, capabilityClipboard)) return false;
    os::IpcMessage request{}, reply{};
    request.type = uint64_t(WindowRequest::ClipboardGet);
    request.data[0] = protocolVersion;
    request.length = 1;
    if (os::callService(service, request, reply) || reply.data[0] ||
        reply.data[1] > clipboardCapacity || reply.data[1] >= capacity ||
        reply.length < 2 + (reply.data[1] + 7) / 8)
        return false;
    length = reply.data[1];
    memcpy(text, &reply.data[2], length);
    text[length] = 0;
    return utf8::valid(text, length);
}
}  // namespace osgui
