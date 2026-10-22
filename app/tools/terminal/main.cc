#include <cstring>
#include <mutex>

#include "os/abi.h"
#include "os/ansi.h"
#include "os/input.h"
#include "os/protocol.h"
#include "os/pty.h"
#include "os/service.h"
#include "os/session.h"
#include "os/task.h"
#include "osgui/gui.h"

namespace {

constexpr uint32_t windowWidth = 704;
constexpr uint32_t windowHeight = 448;
constexpr uint32_t columnCapacity = 86;
uint32_t terminalColumns = columnCapacity;
uint32_t cellWidth = 1, cellHeight = 1;
constexpr uint32_t rowCapacity = 24;
uint32_t terminalRows = rowCapacity;
constexpr uint32_t tabCapacity = 4;
constexpr uint32_t tabWidth = 136;
constexpr uint64_t stackBase = 0x0000000068000000ULL;
constexpr uint64_t stackSize = 64 * 1024;

struct Cell {
    uint32_t character = ' ';
    uint8_t foreground = 7;
    uint8_t background = 0;
    uint8_t attributes = 0;
    uint8_t span = 1;
    char combining[17]{};
};

struct TerminalTab {
    os::PtyHandles pty;
    os::TaskHandle shell;
    Cell cells[rowCapacity][columnCapacity];
    uint32_t cursorX;
    uint32_t cursorY;
    os::terminal::EscapeParser parser;
    os::terminal::Style style;
    bool active;
    char utf8[4]{};
    uint32_t utf8Length = 0, utf8Expected = 0;
};

uint64_t windowService;
osgui::WindowHandle windowHandle;
TerminalTab tabs[tabCapacity];
uint32_t tabCount;
uint32_t activeTab;
std::mutex terminalLock;
os::SessionConfiguration sessionConfig;
bool dirty = true;
Cell painted[rowCapacity][columnCapacity];
uint32_t paintedTab = tabCapacity;
uint32_t paintedTabCount;
uint32_t paintedCursorX;
uint32_t paintedCursorY;

void clearTab(TerminalTab& tab) {
    for (uint32_t row = 0; row < terminalRows; ++row) {
        for (uint32_t column = 0; column < terminalColumns; ++column) {
            tab.cells[row][column] = {};
        }
    }
    tab.cursorX = 0;
    tab.cursorY = 0;
}

void scroll(TerminalTab& tab) {
    for (uint32_t row = 1; row < terminalRows; ++row) {
        for (uint32_t column = 0; column < terminalColumns; ++column) {
            tab.cells[row - 1][column] = tab.cells[row][column];
        }
    }
    for (uint32_t column = 0; column < terminalColumns; ++column) {
        tab.cells[terminalRows - 1][column] = {};
    }
    tab.cursorY = terminalRows - 1;
}

void newLine(TerminalTab& tab) {
    tab.cursorX = 0;
    if (++tab.cursorY == terminalRows) scroll(tab);
}

Cell blank(const TerminalTab& tab) {
    return {' ', tab.style.foreground(), tab.style.background(),
            tab.style.attributes()};
}

unsigned characterWidth(uint32_t c) {
    if ((c >= 0x300 && c <= 0x36f) || (c >= 0x1ab0 && c <= 0x1aff) ||
        (c >= 0x1dc0 && c <= 0x1dff) || (c >= 0x20d0 && c <= 0x20ff) ||
        (c >= 0xfe00 && c <= 0xfe0f) || (c >= 0xfe20 && c <= 0xfe2f))
        return 0;
    return (c >= 0x1100 && c <= 0x115f) || (c >= 0x2e80 && c <= 0xa4cf) ||
                   (c >= 0xac00 && c <= 0xd7a3) || (c >= 0xf900 && c <= 0xfaff) ||
                   (c >= 0xfe10 && c <= 0xfe6f) || (c >= 0xff00 && c <= 0xff60) ||
                   (c >= 0xffe0 && c <= 0xffe6) || (c >= 0x1f300 && c <= 0x1faff) ||
                   (c >= 0x20000 && c <= 0x3ffff)
               ? 2
               : 1;
}
void putCodepoint(TerminalTab& tab, uint32_t c) {
    unsigned span = characterWidth(c);
    if (!span) {
        uint32_t row = tab.cursorY, column = tab.cursorX;
        if (!column) {
            if (!row) return;
            --row;
            column = terminalColumns;
        }
        --column;
        if (!tab.cells[row][column].span && column) --column;
        auto& cell = tab.cells[row][column];
        char encoded[4];
        size_t length = osgui::utf8::encode(c, encoded), used = strlen(cell.combining);
        if (used + length < sizeof(cell.combining)) {
            memcpy(cell.combining + used, encoded, length);
            cell.combining[used + length] = 0;
        }
        return;
    }
    if (span == 2 && tab.cursorX + 1 >= terminalColumns) newLine(tab);
    auto& cell = tab.cells[tab.cursorY][tab.cursorX];
    if (!cell.span && tab.cursorX) tab.cells[tab.cursorY][tab.cursorX - 1] = blank(tab);
    if (cell.span == 2 && tab.cursorX + 1 < terminalColumns)
        tab.cells[tab.cursorY][tab.cursorX + 1] = blank(tab);
    cell = {c, tab.style.foreground(), tab.style.background(), tab.style.attributes(),
            uint8_t(span)};
    if (span == 2)
        tab.cells[tab.cursorY][tab.cursorX + 1] = {0, cell.foreground, cell.background,
                                                   cell.attributes, 0};
    tab.cursorX += span;
    if (tab.cursorX >= terminalColumns) newLine(tab);
}

void consumeOutput(TerminalTab& tab, const uint8_t* bytes, size_t size) {
    for (size_t index = 0; index < size; ++index) {
        const uint8_t value = bytes[index];
        if (value >= 0x80 || tab.utf8Length) {
            if (!tab.utf8Length) {
                tab.utf8Expected = value >= 0xc2 && value <= 0xdf   ? 2
                                   : value >= 0xe0 && value <= 0xef ? 3
                                   : value >= 0xf0 && value <= 0xf4 ? 4
                                                                    : 0;
                if (!tab.utf8Expected) {
                    putCodepoint(tab, 0xfffd);
                    continue;
                }
            } else if ((value & 0xc0) != 0x80) {
                tab.utf8Length = 0;
                putCodepoint(tab, 0xfffd);
                if (value >= 0x80) continue;
            }
            if (value >= 0x80) {
                tab.utf8[tab.utf8Length++] = char(value);
                if (tab.utf8Length == tab.utf8Expected) {
                    size_t offset = 0;
                    uint32_t c;
                    putCodepoint(tab,
                                 osgui::utf8::next(tab.utf8, tab.utf8Length, offset, c)
                                     ? c
                                     : 0xfffd);
                    tab.utf8Length = 0;
                }
                continue;
            }
        }
        const auto result = tab.parser.feed(value);
        if (result == os::terminal::EscapeParser::Result::Pending) continue;
        if (result == os::terminal::EscapeParser::Result::Command) {
            const uint32_t first = tab.parser.parameter(0);
            const uint32_t count = first ? first : 1;
            switch (tab.parser.command()) {
                case 'm':
                    for (uint32_t i = 0; i < tab.parser.count(); ++i)
                        tab.style.apply(tab.parser.parameter(i));
                    break;
                case 'J':
                    if (first == 0 || first == 2)
                        for (auto& row : tab.cells)
                            for (auto& cell : row) cell = blank(tab);
                    break;
                case 'H':
                case 'f': {
                    tab.cursorY = count > terminalRows ? terminalRows - 1 : count - 1;
                    const uint32_t column =
                        tab.parser.parameter(1) ? tab.parser.parameter(1) : 1;
                    tab.cursorX =
                        column > terminalColumns ? terminalColumns - 1 : column - 1;
                    break;
                }
                case 'A':
                    tab.cursorY = count > tab.cursorY ? 0 : tab.cursorY - count;
                    break;
                case 'B':
                    tab.cursorY = tab.cursorY + count < terminalRows
                                      ? tab.cursorY + count
                                      : terminalRows - 1;
                    break;
                case 'C':
                    tab.cursorX = tab.cursorX + count < terminalColumns
                                      ? tab.cursorX + count
                                      : terminalColumns - 1;
                    break;
                case 'D':
                    tab.cursorX = count > tab.cursorX ? 0 : tab.cursorX - count;
                    break;
                case 'K':
                    for (uint32_t column = tab.cursorX; column < terminalColumns;
                         ++column)
                        tab.cells[tab.cursorY][column] = blank(tab);
                    break;
            }
            continue;
        }
        if (value == '\r')
            tab.cursorX = 0;
        else if (value == '\n')
            newLine(tab);
        else if (value == 8 || value == 0x7f) {
            if (tab.cursorX) --tab.cursorX;
        } else if (value == '\t') {
            do {
                tab.cells[tab.cursorY][tab.cursorX++] = blank(tab);
                if (tab.cursorX == terminalColumns) newLine(tab);
            } while ((tab.cursorX & 7U) != 0);
        } else if (value >= 32) {
            putCodepoint(tab, value);
        }
    }
}

osgui::Canvas* currentCanvas = nullptr;
void drawLabel(int32_t x, int32_t y, const char* text, uint32_t color) {
    if (currentCanvas)
        currentCanvas->text(x, y, text, color, osgui::FontFace::Mono, 16);
}

void render() {
    std::lock_guard<std::mutex> guard(terminalLock);
    if (tabCount == 0 || !dirty) return;
    dirty = false;
    osgui::Canvas canvas(windowService, windowHandle);
    currentCanvas = &canvas;
    const bool full = paintedTab != activeTab || paintedTabCount != tabCount;
    if (full) {
        canvas.fill(0, 0, windowWidth, windowHeight, os::terminal::Palette::color(0));
        for (uint32_t index = 0; index < tabCount; ++index) {
            const uint32_t color = index == activeTab ? 0x00313a43U : 0x00222930U;
            canvas.fill(static_cast<int32_t>(index * tabWidth), 0, tabWidth - 1, 30,
                        color);
            char title[] = "Terminal 1";
            title[9] = static_cast<char>('1' + index);
            drawLabel(static_cast<int32_t>(index * tabWidth + 12), 7, title,
                      0x00e7edf3U);
        }
        const int32_t plusX = static_cast<int32_t>(tabCount * tabWidth);
        canvas.fill(plusX, 0, 36, 30, 0x002b8a65U);
        drawLabel(plusX + 14, 7, "+", 0x00ffffffU);
    }

    const TerminalTab& tab = tabs[activeTab];
    auto changed = [&](uint32_t row, uint32_t column) {
        const Cell& a = tab.cells[row][column];
        const Cell& b = painted[row][column];
        return full || a.character != b.character || a.foreground != b.foreground ||
               a.background != b.background || a.attributes != b.attributes ||
               a.span != b.span || strcmp(a.combining, b.combining) ||
               (row == paintedCursorY && column == paintedCursorX) ||
               (row == tab.cursorY && column == tab.cursorX);
    };
    char line[1024];
    for (uint32_t row = 0; row < terminalRows; ++row) {
        for (uint32_t start = 0; start < terminalColumns;) {
            if (!changed(row, start) || !tab.cells[row][start].span) {
                ++start;
                continue;
            }
            const Cell& cell = tab.cells[row][start];
            uint32_t count = 0, columns = 0;
            size_t length = 0;
            while (start + columns < terminalColumns && count < 47) {
                const Cell& next = tab.cells[row][start + columns];
                if (next.foreground != cell.foreground ||
                    next.background != cell.background ||
                    next.attributes != cell.attributes ||
                    !changed(row, start + columns) || !next.span ||
                    (count && (next.span == 2 || cell.span == 2)))
                    break;
                char encoded[4];
                size_t bytes = osgui::utf8::encode(next.character, encoded);
                memcpy(line + length, encoded, bytes);
                length += bytes;
                bytes = strlen(next.combining);
                memcpy(line + length, next.combining, bytes);
                length += bytes;
                columns += next.span;
                ++count;
                if (next.span == 2) break;
            }
            line[length] = 0;
            uint32_t foreground =
                os::terminal::Palette::color(cell.foreground, cell.attributes & 1U);
            uint32_t background = os::terminal::Palette::color(cell.background);
            if (cell.attributes & 4U) {
                uint32_t tmp = foreground;
                foreground = background;
                background = tmp;
            }
            const int32_t x = 8 + int32_t(start * cellWidth),
                          y = 38 + int32_t(row * cellHeight);
            canvas.fill(x, y, columns * cellWidth, cellHeight, background);
            drawLabel(x, y, line, foreground);
            if (cell.attributes & 2U)
                canvas.fill(x, y + int32_t(cellHeight) - 2, columns * cellWidth, 1,
                            foreground);
            start += columns;
        }
    }
    canvas.fill(8 + static_cast<int32_t>(tab.cursorX * cellWidth),
                38 + static_cast<int32_t>(tab.cursorY * cellHeight), cellWidth, 2,
                0x0067d6a3U);
    const bool complete = canvas.flush(true);
    currentCanvas = nullptr;
    if (!complete) {
        dirty = true;
        return;
    }
    for (uint32_t row = 0; row < terminalRows; ++row)
        for (uint32_t column = 0; column < terminalColumns; ++column)
            painted[row][column] = tab.cells[row][column];
    paintedTab = activeTab;
    paintedTabCount = tabCount;
    paintedCursorX = tab.cursorX;
    paintedCursorY = tab.cursorY;
}

bool startSession(TerminalTab& tab) {
    if (!os::Pty::createPty(tab.pty)) return false;
    const char* arguments[] = {sessionConfig.loginName};
    tab.shell =
        os::Task::executeWithIo(sessionConfig.login, 1, arguments, tab.pty.slaveHandle,
                                tab.pty.slaveHandle, tab.pty.slaveHandle);
    os::closeHandle(tab.pty.slaveHandle);
    tab.pty.slaveHandle = 0;
    os::TaskInfo info{};
    if (!tab.shell || os::Task::getInfo(tab.shell, info) ||
        !os::Pty::setPtyForegroundGroup(tab.pty.masterHandle, info.id)) {
        if (tab.shell) {
            os::Task::terminate(tab.shell, -15);
            os::closeHandle(tab.shell);
            tab.shell = 0;
        }
        os::closeHandle(tab.pty.masterHandle);
        tab.pty.masterHandle = 0;
        return false;
    }
    const os::TerminalWindowSize size{terminalRows, terminalColumns, windowWidth,
                                      windowHeight - 30};
    os::Pty::setPtyWindowSize(tab.pty.masterHandle, size);
    os::Pty::setPtyNonblocking(tab.pty.masterHandle, true);
    return true;
}

void readTab(uint32_t index) {
    uint8_t buffer[256];
    while (true) {
        const int64_t count =
            os::Pipe::readPipe(tabs[index].pty.masterHandle, buffer, sizeof(buffer));
        if (count == static_cast<int>(os::ErrorCode::WouldBlock)) {
            int code = 0;
            const int state = os::Task::getState(tabs[index].shell, code);
            if (state == 0 || state == 2) {
                os::sleep(10);
                continue;
            }
        }
        if (count <= 0) {
            os::System::log("[TERMINAL] session exit observed\n");
            int code = 0;
            while (true) {
                const int state = os::Task::getState(tabs[index].shell, code);
                if (state != 0 && state != 2) break;
                os::sleep(10);
            }
            os::TaskInfo info{};
            if (!os::Task::getInfo(tabs[index].shell, info)) {
                os::SessionCleanup::terminate(info.sessionId);
                int status = 0;
                os::Task::waitChild(info.id, status, 1);
            }
            os::closeHandle(tabs[index].shell);
            tabs[index].shell = 0;
            {
                std::lock_guard<std::mutex> guard(terminalLock);
                os::closeHandle(tabs[index].pty.masterHandle);
                tabs[index].pty.masterHandle = 0;
                tabs[index].active = false;
                static const uint8_t exited[] = "\r\n[session ended]\r\n";
                consumeOutput(tabs[index], exited, sizeof(exited) - 1);
                dirty = true;
            }
            if (!code) break;
            bool restored = false;
            for (unsigned attempt = 0; attempt < 5 && !restored; ++attempt) {
                os::sleep(100U << attempt);
                std::lock_guard<std::mutex> guard(terminalLock);
                restored = startSession(tabs[index]);
                if (restored) {
                    tabs[index].active = true;
                    tabs[index].parser = {};
                    tabs[index].style = {};
                    dirty = true;
                    os::System::log("[TERMINAL] login restored\n");
                }
            }
            if (!restored) break;
            continue;
        }
        {
            std::lock_guard<std::mutex> guard(terminalLock);
            consumeOutput(tabs[index], buffer, static_cast<size_t>(count));
            if (index == activeTab) dirty = true;
        }
    }
    os::exitThread(0);
}

void renderMain() {
    while (true) {
        render();
        os::sleep(16);
    }
}

void readTab0() { readTab(0); }
void readTab1() { readTab(1); }
void readTab2() { readTab(2); }
void readTab3() { readTab(3); }

bool createTab() {
    if (tabCount == tabCapacity) return false;
    const uint32_t index = tabCount;
    TerminalTab& tab = tabs[index];
    clearTab(tab);
    if (!startSession(tab)) return false;
    tab.active = true;
    void (*entries[])() = {readTab0, readTab1, readTab2, readTab3};
    const uint64_t stackAddress = stackBase + stackSize * index;
    if (os::mapMemory(stackAddress, stackSize, 1) != stackAddress ||
        os::createThread(entries[index], stackAddress + stackSize) == 0) {
        tab.active = false;
        os::Task::terminate(tab.shell, -15);
        os::closeHandle(tab.shell);
        tab.shell = 0;
        os::closeHandle(tab.pty.masterHandle);
        tab.pty.masterHandle = 0;
        os::unmapMemory(stackAddress, stackSize);
        return false;
    }
    {
        std::lock_guard<std::mutex> guard(terminalLock);
        ++tabCount;
        activeTab = index;
        dirty = true;
    }
    os::System::log("[TERMINAL] tab started\n");
    return true;
}

char translateKey(os::KeyCode key, uint32_t modifiers) {
    const bool shift = (modifiers & os::keyModifierShift) != 0;
    const bool caps = (modifiers & os::keyModifierCapsLock) != 0;
    if (key >= os::KeyCode::A && key <= os::KeyCode::Z) {
        char value = static_cast<char>('a' + static_cast<uint32_t>(key) -
                                       static_cast<uint32_t>(os::KeyCode::A));
        if ((modifiers & os::keyModifierControl) != 0) {
            return static_cast<char>(value - 'a' + 1);
        }
        return shift != caps ? static_cast<char>(value - 'a' + 'A') : value;
    }
    if (key >= os::KeyCode::Digit1 && key <= os::KeyCode::Digit9) {
        static constexpr char shifted[] = "!@#$%^&*(";
        const uint32_t offset =
            static_cast<uint32_t>(key) - static_cast<uint32_t>(os::KeyCode::Digit1);
        return shift ? shifted[offset] : static_cast<char>('1' + offset);
    }
    switch (key) {
        case os::KeyCode::Digit0:
            return shift ? ')' : '0';
        case os::KeyCode::Space:
            return ' ';
        case os::KeyCode::Enter:
        case os::KeyCode::KeypadEnter:
            return '\n';
        case os::KeyCode::Backspace:
            return 0x7f;
        case os::KeyCode::Tab:
            return '\t';
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
        default:
            return 0;
    }
}

void handleKey(const osgui::WindowEvent& event) {
    std::lock_guard<std::mutex> guard(terminalLock);
    if (!tabs[activeTab].active) return;
    if ((event.modifiers & (os::keyModifierControl | os::keyModifierShift)) ==
        (os::keyModifierControl | os::keyModifierShift)) {
        if (event.keyCode == os::KeyCode::C) {
            char output[osgui::clipboardCapacity];
            size_t length = 0;
            for (uint32_t row = 0; row < terminalRows; ++row) {
                for (uint32_t column = 0;
                     column < terminalColumns && length < sizeof(output); ++column) {
                    const auto& cell = tabs[activeTab].cells[row][column];
                    if (!cell.span) continue;
                    char encoded[4];
                    const size_t bytes = osgui::utf8::encode(cell.character, encoded),
                                 combining = strlen(cell.combining);
                    if (length + bytes + combining > sizeof(output)) break;
                    memcpy(output + length, encoded, bytes);
                    length += bytes;
                    memcpy(output + length, cell.combining, combining);
                    length += combining;
                }
                if (length < sizeof(output)) output[length++] = '\n';
            }
            osgui::Clipboard::set(windowService, output, length);
            return;
        }
        if (event.keyCode == os::KeyCode::V) {
            char input[osgui::clipboardCapacity + 1];
            size_t length = 0;
            if (osgui::Clipboard::get(windowService, input, sizeof(input), length))
                os::Pipe::writePipe(tabs[activeTab].pty.masterHandle, input, length);
            return;
        }
    }

    static constexpr char up[] = "\033[A";
    static constexpr char down[] = "\033[B";
    static constexpr char right[] = "\033[C";
    static constexpr char left[] = "\033[D";
    const char* sequence = nullptr;
    if (event.keyCode == os::KeyCode::Up) sequence = up;
    if (event.keyCode == os::KeyCode::Down) sequence = down;
    if (event.keyCode == os::KeyCode::Right) sequence = right;
    if (event.keyCode == os::KeyCode::Left) sequence = left;
    if (sequence != nullptr) {
        os::Pipe::writePipe(tabs[activeTab].pty.masterHandle, sequence, 3);
        return;
    }
    const char value = translateKey(event.keyCode, event.modifiers);
    if (value != 0) {
        os::Pipe::writePipe(tabs[activeTab].pty.masterHandle, &value, 1);
    }
}

}  // namespace

int main(int, char**) {
    osgui::FontMetrics metrics{};
    if (!osgui::Font::metrics(osgui::FontFace::Mono, 16, metrics)) {
        os::System::log("[TERMINAL] shared TTF unavailable\n");
        return 1;
    }
    cellWidth = metrics.cellWidth;
    cellHeight = metrics.lineHeight;
    if (!cellWidth || !cellHeight) return 1;
    terminalColumns = (windowWidth - 16) / cellWidth;
    terminalRows = (windowHeight - 46) / cellHeight;
    if (terminalColumns > columnCapacity) terminalColumns = columnCapacity;
    if (terminalRows > rowCapacity) terminalRows = rowCapacity;

    if (!sessionConfig.load()) {
        os::System::log("[TERMINAL] invalid /etc/sessions\n");
        return 1;
    }
    const uint64_t registry = os::ServiceRegistry::serviceRegistry();
    windowService =
        os::ServiceRegistry::lookupService(registry, osgui::guiwServiceName, 30000);
    if (windowService == 0) return 1;
    windowHandle = osgui::Window::createWindow(windowService, windowWidth, windowHeight,
                                               "Terminal");
    if (windowHandle == 0 || !createTab()) return 2;
    osgui::Window::moveWindow(windowService, windowHandle, 130, 90);
    osgui::Window::showWindow(windowService, windowHandle);
    render();
    const uint64_t renderStack = stackBase + stackSize * tabCapacity;
    if (os::mapMemory(renderStack, stackSize, 1) != renderStack ||
        os::createThread(renderMain, renderStack + stackSize) == 0)
        return 3;
    while (true) {
        osgui::WindowEvent event{};
        if (!osgui::Window::getNextWindowEvent(windowService, windowHandle, event))
            break;
        if (event.type == osgui::WindowEventType::WindowClose) break;
        if (event.type == osgui::WindowEventType::KeyDown) {
            handleKey(event);
        } else if (event.type == osgui::WindowEventType::MouseButtonDown &&
                   event.y >= 0 && event.y < 30) {
            const uint32_t selected = static_cast<uint32_t>(event.x) / tabWidth;
            if (selected < tabCount) {
                {
                    std::lock_guard<std::mutex> guard(terminalLock);
                    activeTab = selected;
                    dirty = true;
                }
                render();
            } else if (selected == tabCount && createTab()) {
                render();
            }
        } else if (event.type == osgui::WindowEventType::WindowResize) {
            const os::TerminalWindowSize size{
                terminalRows, terminalColumns, static_cast<uint32_t>(event.x),
                static_cast<uint32_t>(event.y > 30 ? event.y - 30 : 1)};
            os::Pty::setPtyWindowSize(tabs[activeTab].pty.masterHandle, size);
            {
                std::lock_guard<std::mutex> guard(terminalLock);
                dirty = true;
            }
            render();
        }
    }
    return 0;
}
