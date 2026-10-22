#pragma once
#include <cstring>

#include "os/abi.h"
#include "os/service.h"
#include "osgui/gui.h"
namespace app {
inline char key(os::KeyCode code, uint32_t modifiers) {
    const bool shift = modifiers & os::keyModifierShift,
               caps = modifiers & os::keyModifierCapsLock;
    if (code >= os::KeyCode::A && code <= os::KeyCode::Z)
        return char((shift != caps ? 'A' : 'a') + uint32_t(code) -
                    uint32_t(os::KeyCode::A));
    if (code >= os::KeyCode::Digit1 && code <= os::KeyCode::Digit9) {
        const auto index = uint32_t(code) - uint32_t(os::KeyCode::Digit1);
        return shift ? "!@#$%^&*("[index] : char('1' + index);
    }
    switch (code) {
        case os::KeyCode::Digit0:
            return shift ? ')' : '0';
        case os::KeyCode::Space:
            return ' ';
        case os::KeyCode::Minus:
            return shift ? '_' : '-';
        case os::KeyCode::Equal:
            return shift ? '+' : '=';
        case os::KeyCode::LeftBracket:
            return shift ? '{' : '[';
        case os::KeyCode::RightBracket:
            return shift ? '}' : ']';
        case os::KeyCode::Semicolon:
            return shift ? ':' : ';';
        case os::KeyCode::Apostrophe:
            return shift ? '"' : '\'';
        case os::KeyCode::Grave:
            return shift ? '~' : '`';
        case os::KeyCode::Backslash:
            return shift ? '|' : '\\';
        case os::KeyCode::Comma:
            return shift ? '<' : ',';
        case os::KeyCode::Period:
            return shift ? '>' : '.';
        case os::KeyCode::Slash:
            return shift ? '?' : '/';
        case os::KeyCode::KeypadPlus:
            return '+';
        case os::KeyCode::KeypadMinus:
            return '-';
        case os::KeyCode::KeypadMultiply:
            return '*';
        case os::KeyCode::KeypadDivide:
            return '/';
        case os::KeyCode::KeypadDecimal:
            return '.';
        default:
            return 0;
    }
}
struct Client {
    uint64_t service = 0;
    osgui::WindowHandle window = 0;
    uint32_t width = 0, height = 0;
    bool open(const char* title, uint32_t w, uint32_t h) {
        service = os::ServiceRegistry::lookupService(
            os::ServiceRegistry::serviceRegistry(), osgui::guiwServiceName, 5000);
        if (!service) return false;
        window = osgui::Window::createWindow(service, w, h, title);
        width = w;
        height = h;
        return window && osgui::Window::showWindow(service, window);
    }
    bool poll(osgui::WindowEvent& event) {
        return osgui::Window::pollWindowEvent(service, window, event);
    }
    void close() {
        if (window) osgui::Window::destroyWindow(service, window);
        window = 0;
    }
};
inline void button(osgui::Canvas& canvas, int32_t x, int32_t y, uint32_t width,
                   const char* label, uint32_t color = 0x2b8a65) {
    canvas.fill(x, y, width, 32, color);
    canvas.text(x + 8, y + 7, label, 0xffffff);
}
inline bool inside(const osgui::WindowEvent& event, int32_t x, int32_t y, uint32_t w,
                   uint32_t h = 32) {
    return event.x >= x && event.y >= y && event.x < x + int32_t(w) &&
           event.y < y + int32_t(h);
}
}  // namespace app
