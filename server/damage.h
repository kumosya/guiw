#pragma once
#include <cstdint>
namespace osgui::detail {
struct Rect {
    int32_t x, y, width, height;
};
inline Rect intersect(Rect a, Rect b) {
    const int64_t left = a.x > b.x ? a.x : b.x, top = a.y > b.y ? a.y : b.y;
    const int64_t ar = int64_t(a.x) + a.width, br = int64_t(b.x) + b.width;
    const int64_t ab = int64_t(a.y) + a.height, bb = int64_t(b.y) + b.height;
    const int64_t right = ar < br ? ar : br, bottom = ab < bb ? ab : bb;
    if (a.width <= 0 || a.height <= 0 || b.width <= 0 || b.height <= 0 ||
        right <= left || bottom <= top)
        return {0, 0, 0, 0};
    return {int32_t(left), int32_t(top), int32_t(right - left), int32_t(bottom - top)};
}
class Damage {
   public:
    static constexpr uint32_t capacity = 32;
    Rect regions[capacity]{};
    uint32_t count = 0;
    void add(Rect input, Rect screen) {
        input = intersect(input, screen);
        if (!input.width || !input.height) return;
        for (uint32_t i = 0; i < count;) {
            Rect old = regions[i];
            const int32_t left = old.x < input.x ? old.x : input.x;
            const int32_t top = old.y < input.y ? old.y : input.y;
            const int32_t right = old.x + old.width > input.x + input.width
                                      ? old.x + old.width
                                      : input.x + input.width;
            const int32_t bottom = old.y + old.height > input.y + input.height
                                       ? old.y + old.height
                                       : input.y + input.height;
            const uint64_t area = uint64_t(right - left) * (bottom - top);
            const uint64_t original =
                uint64_t(old.width) * old.height + uint64_t(input.width) * input.height;
            // Merge overlapping or adjacent updates only when the bounding box
            // adds at most 25% extra work. Distant updates remain independent.
            if (area <= original + original / 4) {
                input = {left, top, right - left, bottom - top};
                regions[i] = regions[--count];
                i = 0;
            } else
                ++i;
        }
        if (count == capacity) {
            regions[0] = screen;
            count = 1;
            return;
        }
        regions[count++] = input;
    }
};
}  // namespace osgui::detail
