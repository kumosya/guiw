#pragma once
#include <cstdint>

#include "os/input.h"
namespace osgui {
using WindowHandle = uint64_t;

enum class WindowLayer : uint32_t { Background, Normal, Overlay };

enum class WindowRequest : uint64_t {
    Create = 0x300,
    Destroy,
    Show,
    Hide,
    Resize,
    Move,
    DrawPixel,
    DrawLine,
    FillRectangle,
    DrawText,
    Present,
    GetEvent,
    GetDisplayInfo,
    SetSessionActive,
    SetLayer,
    Raise,
    SubscribeEvents,
    Negotiate = 0x340,
    DrawBatch,
    ClipboardSet,
    ClipboardGet,
    SetMovable
};

enum class WindowEventType : uint32_t {
    None,
    KeyDown,
    KeyUp,
    MouseMove,
    MouseButtonDown,
    MouseButtonUp,
    MouseWheel,
    WindowResize,
    WindowClose,
    WindowFocus,
    WindowVisibility
};

struct WindowEvent {
    WindowEventType type;
    uint32_t modifiers;
    uint64_t timestamp;
    int32_t x;
    int32_t y;
    int32_t delta;
    os::KeyCode keyCode;
    uint32_t buttons;
    uint32_t reserved;
};

constexpr const char* guiwServiceName = "guiw";
constexpr uint64_t windowEventNotification = 0x57494e4556454e54ULL;

inline constexpr uint32_t protocolVersion = 2;
inline constexpr uint64_t capabilityBatch = 1, capabilityClipboard = 2;
inline constexpr uint32_t batchCommandCapacity = 32, batchTextCapacity = 2048;
inline constexpr uint32_t clipboardCapacity = 3072;
enum class DrawKind : uint32_t { Fill, Text };
struct DrawCommand {
    DrawKind kind;
    int32_t x, y;
    uint32_t width, height, color, offset, length, fontFace, fontSize;
};
struct DrawBatch {
    uint32_t version, count, textLength, present;
    DrawCommand commands[batchCommandCapacity];
    char text[batchTextCapacity];
};
}  // namespace osgui
