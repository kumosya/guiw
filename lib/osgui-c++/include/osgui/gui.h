#pragma once
#include "osgui/font.h"
#include "osgui/protocol.h"
namespace osgui {
class Canvas {
   public:
    Canvas(uint64_t service, WindowHandle window);
    bool fill(int32_t x, int32_t y, uint32_t width, uint32_t height, uint32_t color);
    bool text(int32_t x, int32_t y, const char* text, uint32_t color,
              FontFace face = FontFace::Ui, uint32_t size = 16);
    bool flush(bool present = false);
    bool valid() const { return good_; }

   private:
    uint64_t service_;
    WindowHandle window_;
    DrawBatch batch_{};
    bool good_;
};
class Clipboard {
   public:
    static bool set(uint64_t service, const char* text, size_t length);
    static bool get(uint64_t service, char* text, size_t capacity, size_t& length);
};

class Window {
   public:
    static bool setWindowMovable(uint64_t service, WindowHandle window, bool movable);
    static bool raiseWindow(uint64_t windowService, WindowHandle window);
    static bool setWindowLayer(uint64_t windowService, WindowHandle window,
                               WindowLayer layer);
    static bool setWindowSessionActive(uint64_t windowService, bool active);
    static bool getNextWindowEvent(uint64_t windowService, WindowHandle window,
                                   WindowEvent& event);
    static bool pollWindowEvent(uint64_t windowService, WindowHandle window,
                                WindowEvent& event);
    static bool presentWindow(uint64_t windowService, WindowHandle window);
    static bool drawText(uint64_t windowService, WindowHandle window, int32_t x,
                         int32_t y, const char* text, uint32_t color);
    static bool fillRectangle(uint64_t windowService, WindowHandle window, int32_t x,
                              int32_t y, uint32_t width, uint32_t height,
                              uint32_t color);
    static bool drawLine(uint64_t windowService, WindowHandle window, int32_t x0,
                         int32_t y0, int32_t x1, int32_t y1, uint32_t color);
    static bool drawPixel(uint64_t windowService, WindowHandle window, int32_t x,
                          int32_t y, uint32_t color);
    static bool moveWindow(uint64_t windowService, WindowHandle window, int32_t x,
                           int32_t y);
    static bool resizeWindow(uint64_t windowService, WindowHandle window,
                             uint32_t width, uint32_t height);
    static bool hideWindow(uint64_t windowService, WindowHandle window);
    static bool showWindow(uint64_t windowService, WindowHandle window);
    static bool destroyWindow(uint64_t windowService, WindowHandle window);
    static WindowHandle createWindow(uint64_t windowService, uint32_t width,
                                     uint32_t height, const char* title);
};

}  // namespace osgui
