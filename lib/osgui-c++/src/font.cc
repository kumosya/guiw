#include "osgui/font.h"

#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_assert(x) ((void)0)
#include "../vendor/stb_truetype.h"
#include "font-digests.h"
#include "sha256.h"
namespace osgui {
namespace {
constexpr size_t cacheLimit = 512 * 1024;
constexpr uint32_t entryCount = 256;
struct Face {
    stbtt_fontinfo info{};
    uint8_t* data = nullptr;
} faces[3];
struct Glyph {
    uint8_t* pixels = nullptr;
    uint32_t face = 0, id = 0, size = 0;
    int width = 0, height = 0, x = 0, y = 0, advance = 0;
    uint64_t used = 0;
} glyphs[entryCount];
std::mutex fontLock;
size_t allocated = 0;
uint64_t tick = 0;
uint64_t rasterCount = 0;
bool initialized = false, available = false;
bool load(uint32_t index) {
    FILE* input = fopen(fontPaths[index], "rb");
    if (!input) return false;
    if (fseek(input, 0, SEEK_END)) {
        fclose(input);
        return false;
    }
    const long length = ftell(input);
    if (length <= 0 || length > 2 * 1024 * 1024 || fseek(input, 0, SEEK_SET)) {
        fclose(input);
        return false;
    }
    auto* bytes = static_cast<uint8_t*>(malloc(length));
    if (!bytes) {
        fclose(input);
        return false;
    }
    const size_t received = fread(bytes, 1, length, input);
    const int closed = fclose(input);
    uint8_t digest[32];
    detail::sha256(bytes, received, digest);
    // stb_truetype explicitly requires trusted fonts. Authenticate the exact
    // bundled bytes BEFORE allowing the parser to inspect any file offsets.
    if (received != size_t(length) || closed ||
        memcmp(digest, fontDigests[index], 32) ||
        !stbtt_InitFont(&faces[index].info, bytes, 0)) {
        free(bytes);
        return false;
    }
    faces[index].data = bytes;
    return true;
}
bool initializeLocked() {
    if (initialized) return available;
    initialized = true;
    for (uint32_t i = 0; i < 3; ++i) {
        if (load(i))
            available = true;
        else
            fprintf(stderr,
                    "libosgui-c++: missing or invalid shared font: %s; trying fallback\n",
                    fontPaths[i]);
    }
    if (!available)
        fprintf(stderr,
                "libosgui-c++: no verified TTF available; GUI initialization failed\n");
    return available;
}
uint32_t select(FontFace requested) {
    const uint32_t index = uint32_t(requested);
    if (index < 3 && faces[index].data) return index;
    for (uint32_t i = 0; i < 3; ++i)
        if (faces[i].data) return i;
    return 0;
}
void evict(Glyph& glyph) {
    allocated -= size_t(glyph.width) * glyph.height;
    stbtt_FreeBitmap(glyph.pixels, nullptr);
    glyph = {};
}
Glyph* getGlyph(uint32_t requested, uint32_t codepoint, uint32_t size) {
    uint32_t face = requested;
    int id = stbtt_FindGlyphIndex(&faces[face].info, codepoint);
    if (!id)
        for (uint32_t i = 0; i < 3; ++i)
            if (faces[i].data) {
                int candidate = stbtt_FindGlyphIndex(&faces[i].info, codepoint);
                if (candidate) {
                    face = i;
                    id = candidate;
                    break;
                }
            }
    if (!id) id = stbtt_FindGlyphIndex(&faces[face].info, 0xfffd);
    if (!id) id = stbtt_FindGlyphIndex(&faces[face].info, '?');
    for (auto& glyph : glyphs)
        if (glyph.used && glyph.face == face && glyph.id == uint32_t(id) &&
            glyph.size == size) {
            glyph.used = ++tick;
            return &glyph;
        }
    Glyph* slot = &glyphs[0];
    for (auto& glyph : glyphs)
        if (glyph.used < slot->used) slot = &glyph;
    if (slot->used) evict(*slot);
    const auto& font = faces[face].info;
    const float scale = stbtt_ScaleForPixelHeight(&font, float(size));
    slot->pixels = stbtt_GetGlyphBitmap(&font, scale, scale, id, &slot->width,
                                        &slot->height, &slot->x, &slot->y);
    ++rasterCount;
    if (!slot->pixels && slot->width && slot->height) {
        *slot = {};
        return nullptr;
    }
    if (slot->width < 0 || slot->height < 0 || slot->width > 128 ||
        slot->height > 128) {
        stbtt_FreeBitmap(slot->pixels, nullptr);
        *slot = {};
        return nullptr;
    }
    const size_t bytes = size_t(slot->width) * slot->height;
    while (allocated + bytes > cacheLimit) {
        Glyph* oldest = nullptr;
        for (auto& glyph : glyphs)
            if (&glyph != slot && glyph.used && (!oldest || glyph.used < oldest->used))
                oldest = &glyph;
        if (!oldest) break;
        evict(*oldest);
    }
    int advance, bearing;
    stbtt_GetGlyphHMetrics(&font, id, &advance, &bearing);
    slot->advance = int(advance * scale + 0.5f);
    slot->face = face;
    slot->id = id;
    slot->size = size;
    slot->used = ++tick;
    allocated += bytes;
    return slot;
}
bool metricsLocked(FontFace face, uint32_t size, FontMetrics& result) {
    if (size < 8 || size > 48 || uint32_t(face) > 2 || !initializeLocked())
        return false;
    const uint32_t selected = select(face);
    int ascent, descent, gap, advance, bearing;
    const auto& font = faces[selected].info;
    const float scale = stbtt_ScaleForPixelHeight(&font, float(size));
    stbtt_GetFontVMetrics(&font, &ascent, &descent, &gap);
    stbtt_GetCodepointHMetrics(&font, 'M', &advance, &bearing);
    result = {int(ascent * scale + 0.999f), int(-descent * scale + 0.999f),
              int((ascent - descent + gap) * scale + 0.999f),
              int(advance * scale + 0.5f)};
    return true;
}
}  // namespace
TextBitmap::~TextBitmap() { free(pixels); }
bool Font::initialize() {
    std::lock_guard<std::mutex> guard(fontLock);
    return initializeLocked();
}
bool Font::metrics(FontFace face, uint32_t size, FontMetrics& result) {
    std::lock_guard<std::mutex> guard(fontLock);
    return metricsLocked(face, size, result);
}
size_t Font::cacheBytes() {
    std::lock_guard<std::mutex> guard(fontLock);
    return allocated;
}
uint64_t Font::rasterizations() {
    std::lock_guard<std::mutex> guard(fontLock);
    return rasterCount;
}
bool Font::render(const char* text, size_t length, FontFace face, uint32_t size,
                  TextBitmap& result) {
    if (length > 4096 || !utf8::valid(text, length)) return false;
    std::lock_guard<std::mutex> guard(fontLock);
    FontMetrics metrics{};
    if (!metricsLocked(face, size, metrics)) return false;
    const uint32_t selected = select(face);
    int pen = 0, right = 0;
    size_t offset = 0;
    uint32_t c;
    while (offset < length) {
        if (!utf8::next(text, length, offset, c)) return false;
        auto* glyph = getGlyph(selected, c, size);
        if (!glyph) return false;
        const int edge = pen + glyph->x + glyph->width;
        if (edge > right) right = edge;
        pen += glyph->advance;
        if (pen > 1536 || right > 1536) return false;
    }
    const uint32_t width = uint32_t(right > pen ? right : pen),
                   height = metrics.lineHeight;
    auto* bitmap = static_cast<uint8_t*>(calloc(width ? width : 1, height));
    if (!bitmap) return false;
    offset = 0;
    pen = 0;
    while (offset < length) {
        utf8::next(text, length, offset, c);
        auto* glyph = getGlyph(selected, c, size);
        if (!glyph) {
            free(bitmap);
            return false;
        }
        for (int row = 0; row < glyph->height; ++row)
            for (int column = 0; column < glyph->width; ++column) {
                const int x = pen + glyph->x + column,
                          y = metrics.ascent + glyph->y + row;
                if (x >= 0 && y >= 0 && uint32_t(x) < width && uint32_t(y) < height) {
                    const uint8_t alpha = glyph->pixels[row * glyph->width + column];
                    uint8_t& target = bitmap[uint32_t(y) * width + x];
                    target =
                        uint8_t(alpha + (uint32_t(target) * (255 - alpha) + 127) / 255);
                }
            }
        pen += glyph->advance;
    }
    free(result.pixels);
    result.pixels = bitmap;
    result.width = width;
    result.height = height;
    result.advance = pen;
    return true;
}
}  // namespace osgui
