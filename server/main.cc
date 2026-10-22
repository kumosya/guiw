#include <cstdlib>
#include <cstring>
#include <mutex>

#include "batch.h"
#include "damage.h"
#include "os/abi.h"
#include "os/input.h"
#include "os/service.h"
#include "os/sync.h"
#include "os/task.h"
#include "osgui/font.h"
#include "osgui/gui.h"
#include "pixels.h"
#include "profile.h"
#include "widget.h"

namespace {

constexpr uint32_t windowCapacity = 8;
constexpr uint32_t eventCapacity = 32;
constexpr uint32_t maximumSurfaceWidth = 704;
constexpr uint32_t maximumSurfaceHeight = 448;
constexpr uint32_t titleBarHeight = 22;
constexpr uint64_t framebufferAddress = 0x0000000050000000ULL;
constexpr uint64_t frameIntervalNanoseconds = 16666667ULL;
constexpr uint64_t compositorStackAddress = 0x0000000062000000ULL;
constexpr uint64_t serviceThreadStackSize = 64 * 1024;

using Rectangle = osgui::detail::Rect;

class Geometry {
   public:
    static bool intersects(const Rectangle& left, const Rectangle& right);
    static Rectangle intersection(const Rectangle& left, const Rectangle& right);
    static int32_t absolute(int32_t value);
};

bool Geometry::intersects(const Rectangle& left, const Rectangle& right) {
    return left.x < right.x + right.width && right.x < left.x + left.width &&
           left.y < right.y + right.height && right.y < left.y + left.height;
}

Rectangle Geometry::intersection(const Rectangle& left, const Rectangle& right) {
    return osgui::detail::intersect(left, right);
}

int32_t Geometry::absolute(int32_t value) { return value < 0 ? -value : value; }

class DisplayDriver {
   public:
    bool init() {
        if (os::Device::mapFramebuffer(framebufferAddress, info_) || !info_.address ||
            !info_.width || !info_.height || info_.width > 4096 ||
            info_.height > 2160 ||
            (info_.bitsPerPixel != 24 && info_.bitsPerPixel != 32) ||
            uint64_t(info_.width) * (info_.bitsPerPixel / 8) > info_.pitch ||
            info_.redPosition > info_.bitsPerPixel - 8 ||
            info_.greenPosition > info_.bitsPerPixel - 8 ||
            info_.bluePosition > info_.bitsPerPixel - 8)
            return false;
        const uint32_t red = 255U << info_.redPosition,
                       green = 255U << info_.greenPosition,
                       blue = 255U << info_.bluePosition;
        if ((red & green) || (red & blue) || (green & blue)) return false;
        pixels_ =
            static_cast<uint32_t*>(calloc(uint64_t(info_.width) * info_.height, 4));
        return pixels_ != nullptr;
    }
    const os::DisplayInfo& info() const { return info_; }
    void drawPixel(int32_t x, int32_t y, uint32_t color) {
        if (x >= 0 && y >= 0 && uint32_t(x) < info_.width && uint32_t(y) < info_.height)
            pixels_[uint64_t(y) * info_.width + x] = color;
    }
    void fillRectangle(int32_t x, int32_t y, uint32_t width, uint32_t height,
                       uint32_t color) {
        const auto clipped =
            Geometry::intersection({x, y, int32_t(width), int32_t(height)},
                                   {0, 0, int32_t(info_.width), int32_t(info_.height)});
        for (int32_t row = clipped.y; row < clipped.y + clipped.height; ++row) {
            uint32_t* output = pixels_ + uint64_t(row) * info_.width + clipped.x;
            for (int32_t column = 0; column < clipped.width; ++column)
                output[column] = color;
        }
    }
    void blendPixel(int32_t x, int32_t y, uint32_t color, uint32_t alpha) {
        if (x < 0 || y < 0 || uint32_t(x) >= info_.width || uint32_t(y) >= info_.height)
            return;
        uint32_t& target = pixels_[uint64_t(y) * info_.width + x];
        const uint32_t r = (((color >> 16) & 255) * alpha +
                            ((target >> 16) & 255) * (255 - alpha) + 127) /
                           255;
        const uint32_t g = (((color >> 8) & 255) * alpha +
                            ((target >> 8) & 255) * (255 - alpha) + 127) /
                           255;
        const uint32_t b =
            ((color & 255) * alpha + (target & 255) * (255 - alpha) + 127) / 255;
        target = (r << 16) | (g << 8) | b;
    }
    void copyRow(int32_t x, int32_t y, const uint32_t* source, uint32_t width) {
        memcpy(pixels_ + uint64_t(y) * info_.width + x, source, width * 4);
    }
    void present(const Rectangle& rectangle) {
        const uint32_t bytes = info_.bitsPerPixel / 8;
        for (int32_t y = rectangle.y; y < rectangle.y + rectangle.height; ++y) {
            const uint32_t* source = pixels_ + uint64_t(y) * info_.width + rectangle.x;
            auto* target = reinterpret_cast<uint8_t*>(info_.address) +
                           uint64_t(y) * info_.pitch + uint64_t(rectangle.x) * bytes;
            if (bytes == 4 && info_.redPosition == 16 && info_.greenPosition == 8 &&
                info_.bluePosition == 0) {
                memcpy(target, source, rectangle.width * 4);
            } else {
                for (int32_t x = 0; x < rectangle.width; ++x, target += bytes) {
                    const uint32_t color = source[x];
                    const uint32_t packed =
                        ((color >> 16) & 255U) << info_.redPosition |
                        ((color >> 8) & 255U) << info_.greenPosition |
                        (color & 255U) << info_.bluePosition;
                    target[0] = packed;
                    target[1] = packed >> 8;
                    target[2] = packed >> 16;
                    if (bytes == 4) target[3] = packed >> 24;
                }
            }
        }
    }

   private:
    os::DisplayInfo info_{};
    uint32_t* pixels_ = nullptr;
};

class Surface : public osgui::widgets::WidgetCanvas {
   public:
    bool resize(uint32_t width, uint32_t height) {
        return pixels_.resize(width, height);
    }
    uint32_t width() const { return pixels_.width(); }
    uint32_t height() const { return pixels_.height(); }
    uint32_t pixel(uint32_t x, uint32_t y) const {
        return pixels_.data()[y * width() + x];
    }
    const uint32_t* row(uint32_t y) const { return pixels_.data() + y * width(); }
    void drawPixel(int32_t x, int32_t y, uint32_t color) {
        if (x >= 0 && y >= 0 && uint32_t(x) < width() && uint32_t(y) < height() &&
            pixels_.writable())
            pixels_.mutableData()[uint32_t(y) * width() + x] = color;
    }
    void fillRectangle(int32_t x, int32_t y, uint32_t w, uint32_t h,
                       uint32_t color) override {
        const auto clipped =
            Geometry::intersection({x, y, int32_t(w), int32_t(h)},
                                   {0, 0, int32_t(width()), int32_t(height())});
        if (!clipped.width || !clipped.height || !pixels_.writable()) return;
        for (int32_t row = clipped.y; row < clipped.y + clipped.height; ++row) {
            uint32_t* output =
                pixels_.mutableData() + uint32_t(row) * width() + clipped.x;
            for (int32_t column = 0; column < clipped.width; ++column)
                output[column] = color;
        }
    }

    bool prepare() { return pixels_.writable(); }
    void blend(int32_t x, int32_t y, const osgui::TextBitmap& bitmap, uint32_t color) {
        if (!pixels_.writable()) return;
        for (uint32_t row = 0; row < bitmap.height; ++row)
            for (uint32_t column = 0; column < bitmap.width; ++column) {
                const int64_t px = int64_t(x) + column, py = int64_t(y) + row;
                if (px < 0 || py < 0 || px >= width() || py >= height()) continue;
                const uint32_t alpha = bitmap.pixels[row * bitmap.width + column];
                uint32_t& target =
                    pixels_.mutableData()[uint32_t(py) * width() + uint32_t(px)];
                const uint32_t r = (((color >> 16) & 255) * alpha +
                                    ((target >> 16) & 255) * (255 - alpha) + 127) /
                                   255;
                const uint32_t g = (((color >> 8) & 255) * alpha +
                                    ((target >> 8) & 255) * (255 - alpha) + 127) /
                                   255;
                const uint32_t b =
                    ((color & 255) * alpha + (target & 255) * (255 - alpha) + 127) /
                    255;
                target = (r << 16) | (g << 8) | b;
            }
    }
    void drawText(int32_t x, int32_t y, const char* text, uint32_t color) override {
        osgui::TextBitmap bitmap;
        if (text &&
            osgui::Font::render(text, strlen(text), osgui::FontFace::Ui, 16, bitmap))
            blend(x, y, bitmap, color);
    }
    bool mask(const osgui::TextBitmap& bitmap) {
        if (!resize(bitmap.width ? bitmap.width : 1, bitmap.height ? bitmap.height : 1))
            return false;
        for (uint32_t y = 0; y < bitmap.height; ++y)
            for (uint32_t x = 0; x < bitmap.width; ++x)
                pixels_.mutableData()[y * width() + x] =
                    bitmap.pixels[y * bitmap.width + x];
        return true;
    }

    void drawLine(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint32_t color) {
        const int32_t deltaX = Geometry::absolute(x1 - x0);
        const int32_t stepX = x0 < x1 ? 1 : -1;
        const int32_t deltaY = -Geometry::absolute(y1 - y0);
        const int32_t stepY = y0 < y1 ? 1 : -1;
        int32_t error = deltaX + deltaY;
        while (true) {
            drawPixel(x0, y0, color);
            if (x0 == x1 && y0 == y1) {
                break;
            }
            const int32_t doubled = 2 * error;
            if (doubled >= deltaY) {
                error += deltaY;
                x0 += stepX;
            }
            if (doubled <= deltaX) {
                error += deltaX;
                y0 += stepY;
            }
        }
    }

   private:
    osgui::detail::Pixels pixels_;
};

class EventQueue {
   public:
    void clear() { count_ = 0; }
    bool empty() const { return count_ == 0; }

    bool push(const osgui::WindowEvent& event) {
        if (count_ != 0 && event.type == osgui::WindowEventType::MouseMove &&
            events_[count_ - 1].type == osgui::WindowEventType::MouseMove) {
            events_[count_ - 1] = event;
            return true;
        }
        if (count_ == eventCapacity) {
            if (event.type == osgui::WindowEventType::MouseMove &&
                events_[count_ - 1].type == osgui::WindowEventType::MouseMove) {
                events_[count_ - 1] = event;
                return true;
            }
            return false;
        }
        events_[count_++] = event;
        return true;
    }

    bool pop(osgui::WindowEvent& event) {
        if (count_ == 0) {
            return false;
        }
        event = events_[0];
        for (uint32_t index = 1; index < count_; ++index) {
            events_[index - 1] = events_[index];
        }
        --count_;
        return true;
    }

   private:
    osgui::WindowEvent events_[eventCapacity]{};
    uint32_t count_ = 0;
};

struct Window {
    uint64_t handle;
    uint64_t ownerPid;
    int32_t x;
    int32_t y;
    bool active;
    bool visible;
    bool movable = true;
    osgui::WindowLayer layer;
    uint64_t zOrder;
    char title[48];
    Surface surface;
    Surface committed;
    Surface titleMask;
    osgui::detail::Damage pendingDamage;
    EventQueue events;
    uint64_t eventEndpoint;
    bool eventWaiting;
};

class GraphicsServer {
   public:
    bool init() { return display_.init(); }

    DisplayDriver& display() { return display_; }

    void compose(Window* windows, uint32_t count, int32_t cursorX, int32_t cursorY,
                 uint64_t focusedWindow, const Rectangle& dirty) {
        const Rectangle screen = {0, 0, static_cast<int32_t>(display_.info().width),
                                  static_cast<int32_t>(display_.info().height)};
        const Rectangle clipped = Geometry::intersection(dirty, screen);
        if (clipped.width == 0 || clipped.height == 0) {
            return;
        }
        display_.fillRectangle(clipped.x, clipped.y,
                               static_cast<uint32_t>(clipped.width),
                               static_cast<uint32_t>(clipped.height), 0x00272a30U);
        uint64_t previousKey = 0;
        for (uint32_t drawn = 0; drawn < count; ++drawn) {
            Window* next = nullptr;
            uint64_t nextKey = ~0ULL;
            for (uint32_t index = 0; index < count; ++index) {
                Window& candidate = windows[index];
                if (!candidate.active || !candidate.visible) {
                    continue;
                }
                const uint64_t key =
                    (static_cast<uint64_t>(candidate.layer) << 56) | candidate.zOrder;
                if (key >= previousKey && key < nextKey) {
                    next = &candidate;
                    nextKey = key;
                }
            }
            if (next == nullptr) {
                break;
            }
            drawWindow(*next, clipped, next->handle == focusedWindow);
            previousKey = nextKey + 1;
        }
        drawCursor(cursorX, cursorY, clipped);
        display_.present(clipped);
    }

   private:
    void drawWindow(const Window& window, const Rectangle& dirty, bool focused) {
        const Rectangle frame = {
            window.x - 2, window.y - static_cast<int32_t>(titleBarHeight) - 2,
            static_cast<int32_t>(window.surface.width()) + 4,
            static_cast<int32_t>(window.surface.height() + titleBarHeight) + 4};
        if (!Geometry::intersects(frame, dirty)) {
            return;
        }
        fillClipped(frame, dirty, focused ? 0x003aa675U : 0x006a7079U);
        const Rectangle title = {window.x,
                                 window.y - static_cast<int32_t>(titleBarHeight),
                                 static_cast<int32_t>(window.surface.width()),
                                 static_cast<int32_t>(titleBarHeight)};
        fillClipped(title, dirty, focused ? 0x001c704dU : 0x00464b52U);
        drawTitle(window.titleMask, window.x + 7,
                  window.y - static_cast<int32_t>(titleBarHeight) + 2, 0x00ffffffU,
                  Geometry::intersection(dirty, title));
        if (window.surface.width() >= 24) {
            const int32_t x = window.x + int32_t(window.surface.width()) - 15;
            const int32_t y = window.y - int32_t(titleBarHeight) + 7;
            for (int32_t i = 0; i < 8; ++i) {
                fillClipped({x + i, y + i, 1, 1}, dirty, 0xffffff);
                fillClipped({x + 7 - i, y + i, 1, 1}, dirty, 0xffffff);
            }
        }
        const Rectangle content = {window.x, window.y,
                                   static_cast<int32_t>(window.surface.width()),
                                   static_cast<int32_t>(window.surface.height())};
        const Rectangle pixels = Geometry::intersection(content, dirty);
        for (int32_t y = pixels.y; y < pixels.y + pixels.height; ++y)
            display_.copyRow(
                pixels.x, y,
                window.surface.row(uint32_t(y - window.y)) + pixels.x - window.x,
                pixels.width);
    }

    void fillClipped(const Rectangle& rectangle, const Rectangle& dirty,
                     uint32_t color) {
        const Rectangle clipped = Geometry::intersection(rectangle, dirty);
        if (clipped.width > 0 && clipped.height > 0) {
            display_.fillRectangle(clipped.x, clipped.y,
                                   static_cast<uint32_t>(clipped.width),
                                   static_cast<uint32_t>(clipped.height), color);
        }
    }

    void drawTitle(const Surface& mask, int32_t x, int32_t y, uint32_t color,
                   const Rectangle& dirty) {
        const Rectangle clipped = Geometry::intersection(
            {x, y, int32_t(mask.width()), int32_t(mask.height())}, dirty);
        for (int32_t py = clipped.y; py < clipped.y + clipped.height; ++py)
            for (int32_t px = clipped.x; px < clipped.x + clipped.width; ++px) {
                const uint32_t alpha = mask.pixel(uint32_t(px - x), uint32_t(py - y));
                // Titles are white; blend against their stable title-bar background.
                display_.blendPixel(px, py, color, alpha);
            }
    }

    void drawCursor(int32_t x, int32_t y, const Rectangle& dirty) {
        for (int32_t offset = -6; offset <= 6; ++offset) {
            if (x + offset >= dirty.x && x + offset < dirty.x + dirty.width &&
                y >= dirty.y && y < dirty.y + dirty.height) {
                display_.drawPixel(x + offset, y, 0x00ffffffU);
            }
            if (x >= dirty.x && x < dirty.x + dirty.width && y + offset >= dirty.y &&
                y + offset < dirty.y + dirty.height) {
                display_.drawPixel(x, y + offset, 0x00ffffffU);
            }
        }
        if (x >= dirty.x && x < dirty.x + dirty.width && y >= dirty.y &&
            y < dirty.y + dirty.height) {
            display_.drawPixel(x, y, 0x00000000U);
        }
    }

    DisplayDriver display_;
};

class WindowServer {
   public:
    bool init() {
        if (!osgui::Font::initialize() || !graphics_.init()) {
            return false;
        }
        damageEndpoint_ = os::Endpoint::create();
        if (!damageEndpoint_) return false;
        cursorX_ = static_cast<int32_t>(graphics_.display().info().width / 2);
        cursorY_ = static_cast<int32_t>(graphics_.display().info().height / 2);
        initializeDesktopLocked();
        return true;
    }

    void handle(const os::IpcMessage& request, os::IpcMessage& reply) {
        profile_.request();
        const auto operation = static_cast<osgui::WindowRequest>(request.type);
        if (request.length > os::ipcDataWordCount || request.handleCount > 4) {
            reply.data[0] = 1;
            reply.length = 1;
            return;
        }
        if (operation != osgui::WindowRequest::SubscribeEvents && request.handleCount) {
            for (uint32_t i = 0; i < request.handleCount; ++i)
                os::closeHandle(request.handles[i]);
            reply.data[0] = 1;
            reply.length = 1;
            return;
        }
        if (operation == osgui::WindowRequest::Negotiate) {
            reply.length = 3;
            reply.data[0] =
                request.length != 1 || request.data[0] != osgui::protocolVersion;
            reply.data[1] = osgui::protocolVersion;
            reply.data[2] = osgui::capabilityBatch | osgui::capabilityClipboard;
            return;
        }
        if (operation == osgui::WindowRequest::DrawBatch) {
            handleBatch(request, reply);
            return;
        }
        if (operation == osgui::WindowRequest::ClipboardSet ||
            operation == osgui::WindowRequest::ClipboardGet) {
            std::lock_guard<std::mutex> guard(lock_);
            reply.length = 1;
            reply.data[0] = 1;
            if (request.length < 1 || request.data[0] != osgui::protocolVersion) return;
            if (operation == osgui::WindowRequest::ClipboardSet) {
                const uint64_t length = request.data[1];
                if (length > osgui::clipboardCapacity ||
                    request.length != 2 + (length + 7) / 8 ||
                    !osgui::utf8::valid(reinterpret_cast<const char*>(&request.data[2]),
                                        length))
                    return;
                memcpy(clipboard_, &request.data[2], length);
                clipboardLength_ = length;
            } else {
                reply.data[1] = clipboardLength_;
                memcpy(&reply.data[2], clipboard_, clipboardLength_);
                reply.length = 2 + (clipboardLength_ + 7) / 8;
            }
            reply.data[0] = 0;
            return;
        }
        uint32_t required = 1;
        switch (operation) {
            case osgui::WindowRequest::Create:
                required = 8;
                break;
            case osgui::WindowRequest::Resize:
            case osgui::WindowRequest::Move:
                required = 3;
                break;
            case osgui::WindowRequest::DrawPixel:
                required = 4;
                break;
            case osgui::WindowRequest::DrawLine:
            case osgui::WindowRequest::FillRectangle:
                required = 6;
                break;
            case osgui::WindowRequest::DrawText:
                required = 10;
                break;
            case osgui::WindowRequest::SetLayer:
            case osgui::WindowRequest::SetMovable:
                required = 2;
                break;
            case osgui::WindowRequest::GetDisplayInfo:
                required = 0;
                break;
            default:
                break;
        }
        if (request.length < required) {
            reply.data[0] = 1;
            reply.length = 1;
            return;
        }
        if ((operation == osgui::WindowRequest::Create ||
             operation == osgui::WindowRequest::Resize) &&
            (request.data[operation == osgui::WindowRequest::Create ? 0 : 1] >
                 maximumSurfaceWidth ||
             request.data[operation == osgui::WindowRequest::Create ? 1 : 2] >
                 maximumSurfaceHeight)) {
            reply.data[0] = 1;
            reply.length = 1;
            return;
        }
        if (operation == osgui::WindowRequest::DrawLine) {
            for (unsigned i = 1; i <= 4; ++i)
                if (int64_t(request.data[i]) < -4096 ||
                    int64_t(request.data[i]) > 4096) {
                    reply.data[0] = 1;
                    reply.length = 1;
                    return;
                }
        }
        if (operation == osgui::WindowRequest::GetEvent) {
            getEvent(request.senderPid, request.data[0], reply);
            return;
        }
        if (operation == osgui::WindowRequest::SubscribeEvents) {
            subscribeEvents(request, reply);
            return;
        }
        if (operation == osgui::WindowRequest::SetSessionActive) {
            std::lock_guard<std::mutex> displayGuard(displayLock_);
            std::lock_guard<std::mutex> guard(lock_);
            os::TaskInfo caller{};
            reply.length = 1;
            reply.data[0] = 1;
            if (os::Task::query(request.senderPid, caller) ||
                caller.serviceRole != uint32_t(os::ServiceRole::Console))
                return;
            sessionActive_ = request.data[0] != 0;
            capturedWindow_ = 0;
            if (sessionActive_) {
                markScreenDirtyLocked();
                nextFrameTime_ = 0;
            }
            reply.data[0] = 0;
            return;
        }
        osgui::TextBitmap preparedText;
        Surface preparedTitle;
        if (operation == osgui::WindowRequest::DrawText) {
            const char* text = reinterpret_cast<const char*>(&request.data[4]);
            size_t length = 0;
            while (length < 48 && text[length]) ++length;
            if (length == 48 || !osgui::Font::render(text, length, osgui::FontFace::Ui,
                                                     16, preparedText)) {
                reply.data[0] = 1;
                reply.length = 1;
                return;
            }
        }
        if (operation == osgui::WindowRequest::Create) {
            const char* text = reinterpret_cast<const char*>(&request.data[2]);
            size_t length = 0;
            while (length < 48 && text[length]) ++length;
            if (length == 48 ||
                !osgui::Font::render(text, length, osgui::FontFace::Ui, 16,
                                     preparedText) ||
                !preparedTitle.mask(preparedText)) {
                reply.data[0] = 1;
                reply.length = 1;
                return;
            }
        }
        std::lock_guard<std::mutex> guard(lock_);
        Window* window = findOwned(request.senderPid, request.data[0]);
        reply.data[0] = 0;
        switch (operation) {
            case osgui::WindowRequest::Create:
                create(request, reply, preparedTitle);
                break;
            case osgui::WindowRequest::Destroy:
                if (window == nullptr) {
                    reply.data[0] = 1;
                } else {
                    markWindowDirtyLocked(*window);
                    if (window->eventEndpoint != 0) {
                        os::closeHandle(window->eventEndpoint);
                        window->eventEndpoint = 0;
                    }
                    window->eventWaiting = false;
                    if (capturedWindow_ == window->handle) capturedWindow_ = 0;
                    window->active = false;
                    window->visible = false;
                    window->surface = Surface{};
                    window->committed = Surface{};
                    window->titleMask = Surface{};
                    if (focusedWindow_ == window->handle) {
                        focusedWindow_ = 0;
                    }
                }
                break;
            case osgui::WindowRequest::Show:
            case osgui::WindowRequest::Hide:
                if (window == nullptr) {
                    reply.data[0] = 1;
                } else {
                    markWindowDirtyLocked(*window);
                    window->visible = operation == osgui::WindowRequest::Show;
                    if (window->visible && !window->committed.width())
                        window->committed = window->surface;
                    if (!window->visible && capturedWindow_ == window->handle)
                        capturedWindow_ = 0;
                    if (window->visible) {
                        setFocusLocked(*window);
                    }
                    markWindowDirtyLocked(*window);
                    osgui::WindowEvent visibility{};
                    visibility.type = osgui::WindowEventType::WindowVisibility;
                    visibility.x = window->visible ? 1 : 0;
                    window->events.push(visibility);
                    notifyEventWaiterLocked(*window);
                }
                break;
            case osgui::WindowRequest::Resize:
                if (window == nullptr) {
                    reply.data[0] = 1;
                } else {
                    markWindowDirtyLocked(*window);
                    if (!window->surface.resize(
                            static_cast<uint32_t>(request.data[1]),
                            static_cast<uint32_t>(request.data[2]))) {
                        reply.data[0] = 1;
                    } else {
                        osgui::WindowEvent event{};
                        event.type = osgui::WindowEventType::WindowResize;
                        event.timestamp = os::Clock::getMonotonicTime();
                        event.x = static_cast<int32_t>(request.data[1]);
                        event.y = static_cast<int32_t>(request.data[2]);
                        window->events.push(event);
                        notifyEventWaiterLocked(*window);
                        clampWindowLocked(*window);
                        markWindowDirtyLocked(*window);
                    }
                }
                break;
            case osgui::WindowRequest::Move:
                if (window == nullptr) {
                    reply.data[0] = 1;
                } else {
                    markWindowDirtyLocked(*window);
                    window->x = static_cast<int32_t>(request.data[1]);
                    window->y = static_cast<int32_t>(request.data[2]);
                    clampWindowLocked(*window);
                    markWindowDirtyLocked(*window);
                }
                break;
            case osgui::WindowRequest::DrawPixel:
                if (window == nullptr) {
                    reply.data[0] = 1;
                } else {
                    window->surface.drawPixel(static_cast<int32_t>(request.data[1]),
                                              static_cast<int32_t>(request.data[2]),
                                              request.data[3]);
                }
                break;
            case osgui::WindowRequest::DrawLine:
                if (window == nullptr) {
                    reply.data[0] = 1;
                } else {
                    window->surface.drawLine(static_cast<int32_t>(request.data[1]),
                                             static_cast<int32_t>(request.data[2]),
                                             static_cast<int32_t>(request.data[3]),
                                             static_cast<int32_t>(request.data[4]),
                                             request.data[5]);
                }
                break;
            case osgui::WindowRequest::FillRectangle:
                if (window == nullptr) {
                    reply.data[0] = 1;
                } else {
                    window->surface.fillRectangle(
                        static_cast<int32_t>(request.data[1]),
                        static_cast<int32_t>(request.data[2]),
                        static_cast<uint32_t>(request.data[3]),
                        static_cast<uint32_t>(request.data[4]), request.data[5]);
                }
                break;
            case osgui::WindowRequest::DrawText:
                if (window == nullptr) {
                    reply.data[0] = 1;
                } else {
                    window->surface.blend(int32_t(request.data[1]),
                                          int32_t(request.data[2]), preparedText,
                                          uint32_t(request.data[3]));
                }
                break;
            case osgui::WindowRequest::Present:
                if (window == nullptr) {
                    reply.data[0] = 1;
                } else {
                    window->committed = window->surface;
                    if (request.length == 5) {
                        auto region = Geometry::intersection(
                            {int32_t(request.data[1]), int32_t(request.data[2]),
                             int32_t(request.data[3]), int32_t(request.data[4])},
                            {0, 0, int32_t(window->surface.width()),
                             int32_t(window->surface.height())});
                        region.x += window->x;
                        region.y += window->y;
                        markDirtyLocked(region);
                    } else
                        markWindowDirtyLocked(*window);
                }
                break;
            case osgui::WindowRequest::GetDisplayInfo:
                reply.data[1] = graphics_.display().info().width;
                reply.data[2] = graphics_.display().info().height;
                reply.data[3] = graphics_.display().info().bitsPerPixel;
                reply.data[4] =
                    static_cast<uint64_t>(graphics_.display().info().format);
                reply.length = 5;
                break;
            case osgui::WindowRequest::SetLayer:
                if (window == nullptr ||
                    request.data[1] >
                        static_cast<uint64_t>(osgui::WindowLayer::Overlay)) {
                    reply.data[0] = 1;
                } else {
                    markWindowDirtyLocked(*window);
                    window->layer = static_cast<osgui::WindowLayer>(request.data[1]);
                    window->zOrder = nextZOrder_++;
                    markWindowDirtyLocked(*window);
                }
                break;
            case osgui::WindowRequest::SetMovable:
                if (!window || request.data[1] > 1)
                    reply.data[0] = 1;
                else {
                    window->movable = request.data[1] != 0;
                    if (!window->movable && capturedWindow_ == window->handle)
                        capturedWindow_ = 0;
                }
                break;
            case osgui::WindowRequest::Raise:
                if (window == nullptr) {
                    reply.data[0] = 1;
                } else {
                    setFocusLocked(*window);
                    markWindowDirtyLocked(*window);
                }
                break;
            default:
                reply.data[0] = 1;
                break;
        }
        if (reply.length == 0) {
            reply.length = 1;
        }
    }

    void handleInput(const os::InputEvent& input) {
        const auto received = os::Clock::getMonotonicTime();
        {
            std::lock_guard<std::mutex> guard(lock_);
            if (!sessionActive_) {
                return;
            }
            if (!pendingInput_) pendingInput_ = received;
            Window* target = find(focusedWindow_);
            if (input.type == os::InputEventType::MouseMove) {
                markCursorDirtyLocked(cursorX_, cursorY_);
                cursorX_ += input.mouse.deltaX;
                cursorY_ += input.mouse.deltaY;
                clampCursorLocked();
                markCursorDirtyLocked(cursorX_, cursorY_);
                if (Window* captured = find(capturedWindow_)) {
                    if (captured->visible) {
                        markWindowDirtyLocked(*captured);
                        captured->x = cursorX_ - dragOffsetX_;
                        captured->y = cursorY_ - dragOffsetY_;
                        clampWindowLocked(*captured);
                        markWindowDirtyLocked(*captured);
                        return;
                    }
                    capturedWindow_ = 0;
                } else
                    capturedWindow_ = 0;
                target = hitTestLocked(cursorX_, cursorY_);
            } else if (input.type == os::InputEventType::MouseButtonDown) {
                Window* hit = hitTestLocked(cursorX_, cursorY_);
                if (hit != nullptr) {
                    setFocusLocked(*hit);
                    target = hit;
                    if ((input.mouse.buttons & 1U) &&
                        hit->layer == osgui::WindowLayer::Normal && cursorY_ < hit->y &&
                        cursorX_ >= hit->x + int32_t(hit->surface.width()) - 20) {
                        if (hit->ownerPid) {
                            osgui::WindowEvent close{};
                            close.type = osgui::WindowEventType::WindowClose;
                            close.timestamp = input.timestamp;
                            hit->events.push(close);
                            notifyEventWaiterLocked(*hit);
                        } else {
                            markWindowDirtyLocked(*hit);
                            hit->visible = false;
                        }
                        return;
                    }
                    if ((input.mouse.buttons & 1U) && hit->movable &&
                        hit->layer == osgui::WindowLayer::Normal && cursorY_ < hit->y) {
                        capturedWindow_ = hit->handle;
                        dragOffsetX_ = cursorX_ - hit->x;
                        dragOffsetY_ = cursorY_ - hit->y;
                        return;
                    }
                }
            } else if (input.type == os::InputEventType::MouseButtonUp &&
                       capturedWindow_) {
                if (!(input.mouse.buttons & 1U)) capturedWindow_ = 0;
                return;
            }
            if (target != nullptr && target->ownerPid == 0) {
                bool changed = false;
                if (target->handle == welcomeWindowHandle_) {
                    const int32_t x = cursorX_ - target->x;
                    const int32_t y = cursorY_ - target->y;
                    if (input.type == os::InputEventType::MouseButtonDown) {
                        changed = actionButton_.handlePointer(x, y, true);
                        changed = inputBox_.handlePointer(x, y, true) || changed;
                    } else if (input.type == os::InputEventType::MouseButtonUp) {
                        const bool activate = actionButton_.pressed() &&
                                              actionButton_.contains(x, y);
                        changed = actionButton_.handlePointer(x, y, false);
                        if (activate) {
                            if (Window* status = find(systemWindowHandle_)) {
                                status->visible = true;
                                setFocusLocked(*status);
                            }
                        }
                    } else if (input.type == os::InputEventType::KeyDown) {
                        changed =
                            inputBox_.handleKey(input.key.keyCode, input.key.modifiers);
                    }
                    if (changed) {
                        renderDesktopWindowLocked(*target);
                        markWindowDirtyLocked(*target);
                    }
                }
            }
            if (target != nullptr && target->ownerPid != 0) {
                osgui::WindowEvent event{};
                event.timestamp = input.timestamp;
                event.x = cursorX_ - target->x;
                event.y = cursorY_ - target->y;
                event.buttons = input.mouse.buttons;
                event.delta = input.mouse.wheel;
                event.keyCode = input.key.keyCode;
                event.modifiers = input.key.modifiers;
                event.type = translateEventType(input.type);
                target->events.push(event);
                notifyEventWaiterLocked(*target);
            }
        }
    }

    void refresh() {
        profile_.wake();
        const auto frameStart = os::Clock::getMonotonicTime();
        profile_.report(frameStart, osgui::detail::Pixels::peakBytes());
        std::lock_guard<std::mutex> displayGuard(displayLock_);
        Window snapshot[windowCapacity];
        osgui::detail::Damage damage;
        int32_t x, y;
        uint64_t focus, input = 0;
        {
            std::lock_guard<std::mutex> guard(lock_);
            const uint64_t now = os::Clock::getMonotonicTime();
            if (now >= nextClientSweep_) {
                reapClientsLocked();
                nextClientSweep_ = now + 250000000;
            }
            for (Window& window : windows_)
                if (window.active) notifyEventWaiterLocked(window);
            if (!sessionActive_ || !damage_.count || now < nextFrameTime_) return;
            for (uint32_t i = 0; i < windowCapacity; ++i) {
                snapshot[i] = windows_[i];
                snapshot[i].surface = windows_[i].committed;
            }
            damage = damage_;
            damage_.count = 0;
            x = cursorX_;
            y = cursorY_;
            focus = focusedWindow_;
            input = pendingInput_;
            pendingInput_ = 0;
            nextFrameTime_ = now + frameIntervalNanoseconds;
        }
        for (uint32_t i = 0; i < damage.count; ++i)
            graphics_.compose(snapshot, windowCapacity, x, y, focus, damage.regions[i]);
        const auto completed = os::Clock::getMonotonicTime();
        uint64_t area = 0;
        for (uint32_t i = 0; i < damage.count; ++i)
            area += uint64_t(damage.regions[i].width) * damage.regions[i].height;
        profile_.frame(completed - frameStart,
                       input && completed >= input ? completed - input : 0, 0, area,
                       area * (graphics_.display().info().bitsPerPixel / 8));
    }
    void waitForDamage() {
        uint64_t deadline;
        {
            std::lock_guard<std::mutex> guard(lock_);
            deadline = nextClientSweep_;
            if (sessionActive_ && damage_.count && nextFrameTime_ < deadline)
                deadline = nextFrameTime_;
        }
        os::IpcMessage event{};
        os::receiveMessage(damageEndpoint_, event, deadline);
    }
    void cancelInput() {
        std::lock_guard<std::mutex> guard(lock_);
        capturedWindow_ = 0;
    }

   private:
    void reapClientsLocked() {
        for (auto& window : windows_)
            if (window.active && window.ownerPid) {
                os::TaskInfo owner{};
                if (!os::Task::query(window.ownerPid, owner) && owner.state != 2)
                    continue;
                markWindowDirtyLocked(window);
                if (window.eventEndpoint) os::closeHandle(window.eventEndpoint);
                window.eventEndpoint = 0;
                window.eventWaiting = false;
                if (capturedWindow_ == window.handle) capturedWindow_ = 0;
                window.active = window.visible = false;
                window.events.clear();
                window.surface = Surface{};
                window.committed = Surface{};
                window.titleMask = Surface{};
                if (focusedWindow_ == window.handle) focusedWindow_ = 0;
            }
    }
    void handleBatch(const os::IpcMessage& request, os::IpcMessage& reply) {
        reply.length = 1;
        reply.data[0] = 1;
        if (request.length != 1 + (sizeof(osgui::DrawBatch) + 7) / 8) return;
        osgui::DrawBatch batch;
        memcpy(&batch, &request.data[1], sizeof(batch));
        if (!osgui::detail::validBatch(batch)) return;
        osgui::TextBitmap text[osgui::batchCommandCapacity];
        for (uint32_t i = 0; i < batch.count; ++i) {
            const auto& command = batch.commands[i];
            if (command.kind == osgui::DrawKind::Text &&
                !osgui::Font::render(batch.text + command.offset, command.length,
                                     osgui::FontFace(command.fontFace),
                                     command.fontSize, text[i]))
                return;
        }
        std::lock_guard<std::mutex> guard(lock_);
        Window* window = findOwned(request.senderPid, request.data[0]);
        if (!window) return;
        Surface& next = window->surface;
        if (!next.prepare()) return;
        for (uint32_t i = 0; i < batch.count; ++i) {
            const auto& command = batch.commands[i];
            uint32_t width = command.width, height = command.height;
            if (command.kind == osgui::DrawKind::Fill)
                next.fillRectangle(command.x, command.y, width, height, command.color);
            else {
                next.blend(command.x, command.y, text[i], command.color);
                width = text[i].width;
                height = text[i].height;
            }
            window->pendingDamage.add(
                {command.x, command.y, int32_t(width), int32_t(height)},
                {0, 0, int32_t(next.width()), int32_t(next.height())});
        }
        window->surface = next;
        if (batch.present) {
            window->committed = next;
            if (window->visible)
                for (uint32_t i = 0; i < window->pendingDamage.count; ++i) {
                    auto region = window->pendingDamage.regions[i];
                    region.x += window->x;
                    region.y += window->y;
                    markDirtyLocked(region);
                }
            window->pendingDamage.count = 0;
        }
        reply.data[0] = 0;
    }
    void initializeDesktopLocked() {
        initializeSystemWindow(windows_[0], 80, 105, 360, 220, "Welcome to osinfo");
        welcomeWindowHandle_ = windows_[0].handle;
        welcomeLabel_.init({20, 20, 280, 20}, "OSINFO DESKTOP", 0x0021262cU);
        inputBox_.init({20, 70, 300, 36}, "Type here");
        actionButton_.init({20, 132, 132, 38}, "OPEN");

        initializeSystemWindow(windows_[1], 485, 175, 260, 160, "System status");
        systemWindowHandle_ = windows_[1].handle;
        systemLabel_.init({18, 22, 220, 20}, "SYSTEM ONLINE", 0x0021262cU);
        detailLabel_.init({18, 58, 220, 20}, "4 CPU  64-BIT", 0x00545c66U);
        renderDesktopWindowLocked(windows_[0]);
        renderDesktopWindowLocked(windows_[1]);
        focusedWindow_ = welcomeWindowHandle_;
    }

    void initializeSystemWindow(Window& window, int32_t x, int32_t y, uint32_t width,
                                uint32_t height, const char* title) {
        window.surface.resize(width, height);
        window.active = true;
        window.visible = true;
        window.layer = osgui::WindowLayer::Normal;
        window.zOrder = nextZOrder_++;
        window.handle = nextHandle_++;
        window.ownerPid = 0;
        window.eventEndpoint = 0;
        window.eventWaiting = false;
        window.x = x;
        window.y = y;
        window.events.clear();
        os::Runtime::copyString(window.title, title, sizeof(window.title));
        osgui::TextBitmap bitmap;
        if (osgui::Font::render(title, strlen(title), osgui::FontFace::Ui, 16, bitmap))
            window.titleMask.mask(bitmap);
        clampWindowLocked(window);
    }

    void renderDesktopWindowLocked(Window& window) {
        window.surface.fillRectangle(0, 0, window.surface.width(),
                                     window.surface.height(), 0x00f4f6f7U);
        if (window.handle == welcomeWindowHandle_) {
            welcomeLabel_.render(window.surface);
            inputBox_.render(window.surface);
            actionButton_.render(window.surface);
        } else if (window.handle == systemWindowHandle_) {
            systemLabel_.render(window.surface);
            detailLabel_.render(window.surface);
            window.surface.fillRectangle(18, 98, 190, 8, 0x00d9dfe3U);
            window.surface.fillRectangle(18, 98, 138, 8, 0x002b8a65U);
        }
        window.committed = window.surface;
    }

    void create(const os::IpcMessage& request, os::IpcMessage& reply,
                const Surface& title) {
        reapClientsLocked();
        const uint32_t width = static_cast<uint32_t>(request.data[0]);
        const uint32_t height = static_cast<uint32_t>(request.data[1]);
        for (uint32_t index = 0; index < windowCapacity; ++index) {
            Window& window = windows_[index];
            if (!window.active && window.surface.resize(width, height)) {
                window.titleMask = title;
                window.pendingDamage.count = 0;
                window.committed = window.surface;
                window.active = true;
                window.visible = false;
                window.layer = osgui::WindowLayer::Normal;
                window.zOrder = nextZOrder_++;
                window.handle = nextHandle_++;
                window.ownerPid = request.senderPid;
                window.eventEndpoint = 0;
                window.eventWaiting = false;
                window.x = 40 + static_cast<int32_t>(index * 28);
                window.y = 70 + static_cast<int32_t>(index * 24);
                window.events.clear();
                os::Runtime::copyString(window.title,
                                        reinterpret_cast<const char*>(&request.data[2]),
                                        sizeof(window.title));
                reply.data[1] = window.handle;
                reply.length = 2;
                return;
            }
        }
        reply.data[0] = 1;
    }

    void getEvent(uint64_t ownerPid, uint64_t handle, os::IpcMessage& reply) {
        osgui::WindowEvent event{};
        std::lock_guard<std::mutex> guard(lock_);
        Window* window = findOwned(ownerPid, handle);
        if (window == nullptr || window->eventEndpoint == 0) {
            reply.data[0] = 1;
            reply.length = 1;
            return;
        }
        if (!window->events.pop(event)) {
            window->eventWaiting = true;
            reply.data[0] =
                static_cast<uint64_t>(static_cast<int64_t>(os::ErrorCode::WouldBlock));
            reply.length = 1;
            return;
        }
        window->eventWaiting = false;
        reply.data[0] = 0;
        const uint8_t* source = reinterpret_cast<const uint8_t*>(&event);
        for (uint32_t index = 0; index < sizeof(event); ++index) {
            reinterpret_cast<uint8_t*>(&reply.data[1])[index] = source[index];
        }
        reply.length = 1 + sizeof(event) / sizeof(uint64_t);
    }

    void subscribeEvents(const os::IpcMessage& request, os::IpcMessage& reply) {
        std::lock_guard<std::mutex> guard(lock_);
        Window* window = findOwned(request.senderPid, request.data[0]);
        if (window == nullptr || request.handleCount != 1) {
            for (uint32_t index = 0; index < request.handleCount; ++index) {
                os::closeHandle(request.handles[index]);
            }
            reply.data[0] = 1;
            reply.length = 1;
            return;
        }
        if (window->eventEndpoint != 0) {
            os::closeHandle(window->eventEndpoint);
        }
        window->eventEndpoint = request.handles[0];
        window->eventWaiting = false;
        reply.data[0] = 0;
        reply.length = 1;
    }

    void notifyEventWaiterLocked(Window& window) {
        if (!window.eventWaiting || window.eventEndpoint == 0 || window.events.empty())
            return;
        os::IpcMessage notification{};
        notification.type = osgui::windowEventNotification;
        // Input, compositor and request processing share this lock. A client
        // that stops draining its endpoint must never block all three threads.
        const int status = os::sendMessage(window.eventEndpoint, notification, 0);
        if (status == 0)
            window.eventWaiting = false;
        else if (status != static_cast<int>(os::ErrorCode::WouldBlock) &&
                 status != static_cast<int>(os::ErrorCode::Timeout) &&
                 status != static_cast<int>(os::ErrorCode::Interrupted)) {
            os::closeHandle(window.eventEndpoint);
            window.eventEndpoint = 0;
            window.eventWaiting = false;
        }
        // A full queue retains eventWaiting; refresh retries after it drains.
    }

    Window* find(uint64_t handle) {
        for (Window& window : windows_) {
            if (window.active && window.handle == handle) {
                return &window;
            }
        }
        return nullptr;
    }

    Window* findOwned(uint64_t ownerPid, uint64_t handle) {
        Window* window = find(handle);
        return window != nullptr && window->ownerPid == ownerPid ? window : nullptr;
    }

    Window* hitTestLocked(int32_t x, int32_t y) {
        Window* result = nullptr;
        uint64_t resultKey = 0;
        for (uint32_t index = 0; index < windowCapacity; ++index) {
            Window& window = windows_[index];
            if (window.active && window.visible && x >= window.x &&
                y >= window.y - static_cast<int32_t>(titleBarHeight) &&
                x < window.x + static_cast<int32_t>(window.surface.width()) &&
                y < window.y + static_cast<int32_t>(window.surface.height())) {
                const uint64_t key =
                    (static_cast<uint64_t>(window.layer) << 56) | window.zOrder;
                if (result == nullptr || key > resultKey) {
                    result = &window;
                    resultKey = key;
                }
            }
        }
        return result;
    }

    void setFocusLocked(Window& window) {
        if (focusedWindow_ == window.handle) {
            markWindowDirtyLocked(window);
            window.zOrder = nextZOrder_++;
            return;
        }
        Window* previous = find(focusedWindow_);
        if (previous != nullptr) {
            markWindowDirtyLocked(*previous);
        }
        focusedWindow_ = window.handle;
        window.zOrder = nextZOrder_++;
        markWindowDirtyLocked(window);
        osgui::WindowEvent event{};
        event.type = osgui::WindowEventType::WindowFocus;
        event.timestamp = os::Clock::getMonotonicTime();
        window.events.push(event);
        notifyEventWaiterLocked(window);
    }

    void clampCursorLocked() {
        if (cursorX_ < 0) cursorX_ = 0;
        if (cursorY_ < 0) cursorY_ = 0;
        const int32_t maximumX =
            static_cast<int32_t>(graphics_.display().info().width) - 1;
        const int32_t maximumY =
            static_cast<int32_t>(graphics_.display().info().height) - 1;
        if (cursorX_ > maximumX) cursorX_ = maximumX;
        if (cursorY_ > maximumY) cursorY_ = maximumY;
    }

    void clampWindowLocked(Window& window) {
        const int32_t screenWidth = int32_t(graphics_.display().info().width);
        const int32_t screenHeight = int32_t(graphics_.display().info().height);
        const int32_t minimumX = 32 - int32_t(window.surface.width());
        const int32_t maximumX = screenWidth - 32;
        const int32_t maximumY =
            screenHeight > int32_t(titleBarHeight) ? screenHeight : titleBarHeight;
        if (window.x < minimumX) window.x = minimumX;
        if (window.x > maximumX) window.x = maximumX;
        if (window.y < int32_t(titleBarHeight)) window.y = titleBarHeight;
        if (window.y > maximumY) window.y = maximumY;
    }

    static osgui::WindowEventType translateEventType(os::InputEventType type) {
        switch (type) {
            case os::InputEventType::KeyDown:
                return osgui::WindowEventType::KeyDown;
            case os::InputEventType::KeyUp:
                return osgui::WindowEventType::KeyUp;
            case os::InputEventType::MouseMove:
                return osgui::WindowEventType::MouseMove;
            case os::InputEventType::MouseButtonDown:
                return osgui::WindowEventType::MouseButtonDown;
            case os::InputEventType::MouseButtonUp:
                return osgui::WindowEventType::MouseButtonUp;
            case os::InputEventType::MouseWheel:
                return osgui::WindowEventType::MouseWheel;
            default:
                return osgui::WindowEventType::None;
        }
    }

    Rectangle windowBounds(const Window& window) const {
        return {window.x - 2, window.y - static_cast<int32_t>(titleBarHeight) - 2,
                static_cast<int32_t>(window.surface.width()) + 4,
                static_cast<int32_t>(window.surface.height() + titleBarHeight) + 4};
    }

    void markWindowDirtyLocked(const Window& window) {
        if (window.active && window.visible) {
            markDirtyLocked(windowBounds(window));
        }
    }

    void markCursorDirtyLocked(int32_t x, int32_t y) {
        markDirtyLocked({x - 6, y - 6, 13, 13});
    }

    void markScreenDirtyLocked() {
        markDirtyLocked({0, 0, static_cast<int32_t>(graphics_.display().info().width),
                         static_cast<int32_t>(graphics_.display().info().height)});
    }

    void markDirtyLocked(const Rectangle& rectangle) {
        const bool empty = damage_.count == 0;
        damage_.add(rectangle, {0, 0, int32_t(graphics_.display().info().width),
                                int32_t(graphics_.display().info().height)});
        if (empty && damage_.count) {
            os::IpcMessage event{};
            os::sendMessage(damageEndpoint_, event, 0);
        }
    }

    GraphicsServer graphics_;
    Window windows_[windowCapacity]{};
    osgui::detail::Profile profile_;
    uint64_t pendingInput_ = 0;
    std::mutex lock_;
    std::mutex displayLock_;
    uint64_t damageEndpoint_ = 0;
    char clipboard_[osgui::clipboardCapacity]{};
    uint32_t clipboardLength_ = 0;
    uint64_t nextHandle_ = 1;
    uint64_t focusedWindow_ = 0;
    uint64_t nextZOrder_ = 1;
    uint64_t nextFrameTime_ = 0;
    uint64_t nextClientSweep_ = 0;
    int32_t cursorX_ = 0;
    int32_t cursorY_ = 0;
    osgui::detail::Damage damage_;
    uint64_t capturedWindow_ = 0;
    int32_t dragOffsetX_ = 0, dragOffsetY_ = 0;
    bool sessionActive_ = false;
    uint64_t welcomeWindowHandle_ = 0;
    uint64_t systemWindowHandle_ = 0;
    osgui::widgets::Label welcomeLabel_;
    osgui::widgets::Label systemLabel_;
    osgui::widgets::Label detailLabel_;
    osgui::widgets::Button actionButton_;
    osgui::widgets::TextBox inputBox_;
};

class WindowApplication {
   public:
    static void inputMain(void* self) {
        static_cast<WindowApplication*>(self)->inputLoop();
    }
    static void compositorMain(void* self) {
        static_cast<WindowApplication*>(self)->compositorLoop();
    }
    WindowServer windowServer;
    uint64_t registryEndpoint;

    void inputLoop() {
        const uint64_t input = os::ServiceRegistry::lookupService(
            registryEndpoint, os::inputServiceName, 5000);
        os::InputClient client;
        if (!client.connect(input)) {
            os::System::log("[GUIW] input service unavailable\n");
            return;
        }
        while (true) {
            if (!client.wait()) {
                windowServer.cancelInput();
                os::System::log("[GUIW] input subscription failed\n");
                return;
            }
            os::InputEvent event{};
            while (client.receive(event)) {
                windowServer.handleInput(event);
            }
        }
    }

    void compositorLoop() {
        while (true) {
            windowServer.refresh();
            windowServer.waitForDamage();
        }
    }

    int run() {
        os::System::log("[GUIW] Window Server started in Ring 3\n");
        registryEndpoint = os::getBootstrapHandle();
        const uint64_t endpoint = os::Endpoint::create();
        if (registryEndpoint == 0 || endpoint == 0 || !windowServer.init() ||
            !os::ServiceRegistry::registerService(registryEndpoint,
                                                  osgui::guiwServiceName, endpoint)) {
            os::System::log("[GUIW] display or service initialization failed\n");
            return 1;
        }
        constexpr uint64_t inputStackAddress = 0x0000000061000000ULL;
        if (os::mapMemory(inputStackAddress, serviceThreadStackSize, 1) !=
                inputStackAddress ||
            os::mapMemory(compositorStackAddress, serviceThreadStackSize, 1) !=
                compositorStackAddress ||
            os::createThread(inputMain, inputStackAddress + serviceThreadStackSize,
                             this) == 0 ||
            os::createThread(compositorMain,
                             compositorStackAddress + serviceThreadStackSize,
                             this) == 0) {
            os::System::log("[GUIW] unable to start input or compositor thread\n");
            return 2;
        }
        os::System::log(
            "[GRAPHICS] linear framebuffer and software compositor online\n");
        os::System::log("[GUIW] layered 60 Hz compositor and input dispatch online\n");
        while (true) {
            os::IpcMessage request{};
            if (os::receiveMessage(endpoint, request) != 0) {
                continue;
            }
            os::IpcMessage reply{};
            windowServer.handle(request, reply);
            os::replyMessage(endpoint, request.replyToken, reply);
        }
    }
};
}  // namespace
int main(int, char**) {
    static WindowApplication application;
    return application.run();
}
