#include "osgui/font.h"
namespace osgui::utf8 {
bool next(const char* text, size_t length, size_t& offset, uint32_t& value) {
    if (!text || offset >= length) return false;
    const auto first = static_cast<uint8_t>(text[offset++]);
    if (first < 128) {
        value = first;
        return true;
    }
    uint32_t count, minimum;
    if (first >= 0xc2 && first <= 0xdf) {
        count = 1;
        value = first & 31;
        minimum = 0x80;
    } else if (first >= 0xe0 && first <= 0xef) {
        count = 2;
        value = first & 15;
        minimum = 0x800;
    } else if (first >= 0xf0 && first <= 0xf4) {
        count = 3;
        value = first & 7;
        minimum = 0x10000;
    } else
        return false;
    if (length - offset < count) return false;
    for (uint32_t i = 0; i < count; ++i) {
        const auto byte = static_cast<uint8_t>(text[offset++]);
        if ((byte & 0xc0) != 0x80) return false;
        value = (value << 6) | (byte & 63);
    }
    return value >= minimum && value <= 0x10ffff &&
           !(value >= 0xd800 && value <= 0xdfff);
}
bool valid(const char* text, size_t length) {
    size_t offset = 0;
    uint32_t value;
    while (offset < length)
        if (!next(text, length, offset, value) || !value) return false;
    return true;
}
size_t previous(const char* text, size_t offset) {
    if (!offset) return 0;
    --offset;
    while (offset && (static_cast<uint8_t>(text[offset]) & 0xc0) == 0x80) --offset;
    return offset;
}
size_t encode(uint32_t c, char output[4]) {
    if (c <= 0x7f) {
        output[0] = c;
        return 1;
    }
    if (c <= 0x7ff) {
        output[0] = 0xc0 | (c >> 6);
        output[1] = 0x80 | (c & 63);
        return 2;
    }
    if (c >= 0xd800 && c <= 0xdfff) return 0;
    if (c <= 0xffff) {
        output[0] = 0xe0 | (c >> 12);
        output[1] = 0x80 | ((c >> 6) & 63);
        output[2] = 0x80 | (c & 63);
        return 3;
    }
    if (c > 0x10ffff) return 0;
    output[0] = 0xf0 | (c >> 18);
    output[1] = 0x80 | ((c >> 12) & 63);
    output[2] = 0x80 | ((c >> 6) & 63);
    output[3] = 0x80 | (c & 63);
    return 4;
}
}  // namespace osgui::utf8
