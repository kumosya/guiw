#pragma once
#include <cstddef>
#include <cstdint>
namespace osgui {
enum class FontFace : uint32_t { Ui, Mono, Fallback };
struct FontMetrics {
    int32_t ascent, descent, lineHeight, cellWidth;
};
struct TextBitmap {
    uint8_t* pixels = nullptr;
    uint32_t width = 0, height = 0;
    int32_t advance = 0;
    TextBitmap() = default;
    ~TextBitmap();
    TextBitmap(const TextBitmap&) = delete;
    TextBitmap& operator=(const TextBitmap&) = delete;
};
class Font {
   public:
    static bool initialize();
    static bool metrics(FontFace face, uint32_t size, FontMetrics& result);
    // Top-left origin, with baseline supplied by FontMetrics::ascent.
    // Only the integrity-checked fonts shipped by libosgui are accepted.
    static bool render(const char* text, size_t length, FontFace face, uint32_t size,
                       TextBitmap& result);
    static size_t cacheBytes();
    static uint64_t rasterizations();
};
namespace utf8 {
bool next(const char* text, size_t length, size_t& offset, uint32_t& codepoint);
bool valid(const char* text, size_t length);
size_t previous(const char* text, size_t offset);
size_t encode(uint32_t codepoint, char output[4]);
}  // namespace utf8
}  // namespace osgui
