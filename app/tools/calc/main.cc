#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>

#include "../../common/client.h"
#include "number.h"
namespace {
app::Client client;
std::mutex lock;
char expression[2049]{};
uint32_t length = 0, precision = 100, generation = 0, pending = 0;
bool running = true, dirty = true, busy = false;
calc::Result result;
const char* buttons[] = {"(", ")",   "C", "Back", "Cancel", "7", "8",   "9", "/",
                         "%", "4",   "5", "6",    "*",      "^", "1",   "2", "3",
                         "-", "+/-", "0", ".",    "=",      "+", "Copy"};
void invalidate() {
    __atomic_add_fetch(&generation, 1, __ATOMIC_ACQ_REL);
    pending = 0;
    busy = false;
    result = {};
    dirty = true;
}
void action(const char* label) {
    if (!strcmp(label, "=")) {
        pending = __atomic_add_fetch(&generation, 1, __ATOMIC_ACQ_REL);
        busy = true;
        dirty = true;
        return;
    }
    if (!strcmp(label, "Cancel")) {
        invalidate();
        return;
    }
    if (!strcmp(label, "Copy")) {
        if (!result.error)
            osgui::Clipboard::set(client.service, result.text.c_str(),
                                  result.text.size());
        return;
    }
    invalidate();
    if (!strcmp(label, "C"))
        length = 0;
    else if (!strcmp(label, "Back")) {
        if (length) --length;
    } else if (!strcmp(label, "+/-")) {
        if (length + 3 < sizeof(expression)) {
            expression[length++] = '*';
            expression[length++] = '-';
            expression[length++] = '1';
        }
    } else if (length + strlen(label) < sizeof(expression)) {
        memcpy(expression + length, label, strlen(label));
        length += strlen(label);
    }
    expression[length] = 0;
}
void worker() {
    while (true) {
        char text[2049];
        uint32_t serial = 0, digits = 0;
        {
            std::lock_guard<std::mutex> guard(lock);
            if (!running) break;
            if (pending) {
                serial = pending;
                pending = 0;
                digits = precision;
                memcpy(text, expression, length + 1);
            }
        }
        if (!serial) {
            os::sleep(10);
            continue;
        }
        auto computed = calc::evaluate(text, digits, &generation, serial);
        std::lock_guard<std::mutex> guard(lock);
        if (serial == __atomic_load_n(&generation, __ATOMIC_ACQUIRE)) {
            result = computed;
            busy = false;
            dirty = true;
        }
    }
}
void render() {
    osgui::Canvas canvas(client.service, client.window);
    canvas.fill(0, 0, 520, 400, 0x20262d);
    canvas.text(12, 10, expression + (length > 65 ? length - 65 : 0), 0xffffff,
                osgui::FontFace::Mono, 16);
    const char* answer = busy           ? "Calculating..."
                         : result.error ? result.error
                                        : result.text.c_str();
    char visible[101];
    snprintf(visible, sizeof(visible), "%s%.64s%s", result.approximate ? "~ " : "",
             answer, strlen(answer) > 64 ? "... [Copy for all digits]" : "");
    canvas.text(12, 42, visible, 0x6fd0aa, osgui::FontFace::Mono, 16);
    char info[96];
    snprintf(info, sizeof(info), "Precision: %u significant digits (1..1000)",
             precision);
    canvas.text(12, 74, info, 0xdddddd);
    app::button(canvas, 396, 67, 48, "P-");
    app::button(canvas, 450, 67, 48, "P+");
    for (unsigned i = 0; i < 25; ++i)
        app::button(canvas, 12 + int(i % 5) * 101, 112 + int(i / 5) * 53, 94,
                    buttons[i]);
    canvas.flush(true);
    dirty = false;
}
}  // namespace
int main(int argc, char** argv) {
    if (argc > 1) {
        unsigned configured = unsigned(atoi(argv[1]));
        if (configured >= 1 && configured <= 1000)
            precision = configured;
        else
            return 2;
    }
    if (!client.open("Calculator", 520, 400)) return 1;
    std::thread thread(worker);
    while (running) {
        osgui::WindowEvent event{};
        while (client.poll(event)) {
            std::lock_guard<std::mutex> guard(lock);
            if (event.type == osgui::WindowEventType::WindowClose) {
                running = false;
                invalidate();
                break;
            }
            if (event.type == osgui::WindowEventType::MouseButtonDown &&
                (event.buttons & 1)) {
                if (app::inside(event, 396, 67, 48)) {
                    if (precision > 1) --precision;
                    invalidate();
                } else if (app::inside(event, 450, 67, 48)) {
                    if (precision < 1000) ++precision;
                    invalidate();
                }
                for (unsigned i = 0; i < 25; ++i)
                    if (app::inside(event, 12 + int(i % 5) * 101, 112 + int(i / 5) * 53,
                                    94))
                        action(buttons[i]);
            }
            if (event.type == osgui::WindowEventType::KeyDown) {
                if (event.keyCode == os::KeyCode::Enter ||
                    event.keyCode == os::KeyCode::KeypadEnter)
                    action("=");
                else if (event.keyCode == os::KeyCode::Escape)
                    action("Cancel");
                else if (event.keyCode == os::KeyCode::Backspace)
                    action("Back");
                else {
                    char text[2] = {app::key(event.keyCode, event.modifiers), 0};
                    if (text[0] && strchr("0123456789.()+-*/%^ ", text[0]))
                        action(text);
                }
            }
        }
        {
            std::lock_guard<std::mutex> guard(lock);
            if (dirty) render();
        }
        os::sleep(16);
    }
    thread.join();
    client.close();
    return 0;
}
