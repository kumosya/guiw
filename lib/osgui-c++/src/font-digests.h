#pragma once
namespace osgui {
#ifndef OSGUI_FONT_DIR
#define OSGUI_FONT_DIR "/usr/share/fonts/osinfo/"
#endif
constexpr const char* fontPaths[3] = {
    OSGUI_FONT_DIR "ui.ttf", OSGUI_FONT_DIR "mono.ttf", OSGUI_FONT_DIR "fallback.ttf"};
constexpr uint8_t fontDigests[3][32] = {
    {87, 247, 62,  17,  245, 25,  153, 67, 43,  247, 171, 34, 206, 85, 182, 249,
     69, 213, 236, 161, 191, 130, 68,  4,  207, 169, 236, 46, 55,  24, 200, 78},
    {84, 191, 130, 126, 185, 148, 4, 232, 244, 48,  195, 48, 173, 48,  240, 99,
     51, 79,  99,  126, 186, 1,   9, 182, 161, 141, 79,  86, 106, 142, 157, 216},
    {140, 178, 159, 125, 178, 80, 235, 178, 85,  26, 108, 226, 193, 224, 191, 213,
     160, 235, 82,  14,  158, 35, 51,  112, 219, 4,  147, 232, 46,  31,  54,  247},
};
}  // namespace osgui
