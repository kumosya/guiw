#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>

#include "../../common/client.h"
#include "process.h"
namespace {
app::Client client;
std::mutex lock;
taskmgr::Snapshot snapshot;
bool running = true, dirty = true, refresh = true, visible = true;
uint32_t sort = 0, scroll = 0;
uint64_t selected = 0;
char filter[64]{}, message[160]{};
os::TaskInfo target{};
bool confirming = false, force = false;
char painted[16][112]{};
uint64_t paintedPid[16]{};
bool paintedSelected[16]{};
bool paintedValid = false;
void worker() {
    auto* previous = new taskmgr::Snapshot;
    auto* result = new taskmgr::Snapshot;
    if (!previous || !result) {
        delete previous;
        delete result;
        return;
    }
    while (true) {
        bool update = false;
        bool shown;
        {
            std::lock_guard<std::mutex> guard(lock);
            if (!running) break;
            update = refresh;
            refresh = false;
            shown = visible;
        }
        static uint64_t next = 0;
        const uint64_t now = os::Clock::getMonotonicTime();
        if (update || now >= next) {
            taskmgr::sample(*result, *previous);
            *previous = *result;
            {
                std::lock_guard<std::mutex> guard(lock);
                snapshot = *result;
                dirty = true;
            }
            next = now + (shown ? 1000000000ULL : 5000000000ULL);
        }
        os::sleep(25);
    }
    delete previous;
    delete result;
}
void render() {
    uint32_t indices[taskmgr::capacity], count = 0;
    for (uint32_t i = 0; i < snapshot.count; ++i) {
        char pid[32];
        snprintf(pid, sizeof(pid), "%llu",
                 (unsigned long long)snapshot.rows[i].info.id);
        if (!filter[0] || strstr(snapshot.rows[i].info.name, filter) ||
            strstr(pid, filter))
            indices[count++] = i;
    }
    for (uint32_t i = 1; i < count; ++i)
        for (uint32_t j = i; j > 0; --j) {
            const auto& a = snapshot.rows[indices[j - 1]];
            const auto& b = snapshot.rows[indices[j]];
            const bool swap = sort == 0   ? a.info.id > b.info.id
                              : sort == 1 ? strcmp(a.info.name, b.info.name) > 0
                                          : a.cpu < b.cpu;
            if (!swap) break;
            uint32_t tmp = indices[j];
            indices[j] = indices[j - 1];
            indices[j - 1] = tmp;
        }
    if (scroll >= count) scroll = count ? count - 1 : 0;
    osgui::Canvas canvas(client.service, client.window);
    if (!paintedValid) canvas.fill(0, 0, 704, 448, 0x20262d);
    canvas.fill(0, 0, 704, 86, 0x20262d);
    char heading[128];
    snprintf(heading, sizeof(heading), "Filter (type): %s  Sort: %s", filter,
             sort == 0   ? "PID"
             : sort == 1 ? "Name"
                         : "CPU");
    canvas.text(10, 8, heading, 0xffffff);
    app::button(canvas, 10, 36, 90, "Refresh");
    app::button(canvas, 110, 36, 80, "Sort");
    app::button(canvas, 200, 36, 112, "End (TERM)");
    app::button(canvas, 322, 36, 112, "Force (KILL)", 0xa54040);
    canvas.text(10, 70, "PID   PPID  UID STATE CPU%   CPU ms  NAME", 0x6fd0aa,
                osgui::FontFace::Mono, 14);
    for (uint32_t row = 0; row < 16; ++row) {
        char text[112]{};
        uint64_t id = 0;
        if (scroll + row < count) {
            const auto& item = snapshot.rows[indices[scroll + row]];
            id = item.info.id;
            char cpu[24];
            if (item.sampled)
                snprintf(cpu, sizeof(cpu), "%u.%02u", item.cpu / 100, item.cpu % 100);
            else
                strcpy(cpu, "wait");
            snprintf(text, sizeof(text), "%5llu %5llu %4u %5u %7s %8llu %.35s%s",
                     (unsigned long long)id, (unsigned long long)item.info.parentId,
                     item.info.uid, item.info.state, cpu,
                     (unsigned long long)(item.info.cpuNanoseconds / 1000000),
                     item.info.name, item.info.managedService ? " [service]" : "");
        }
        if (!paintedValid || strcmp(painted[row], text) || paintedPid[row] != id ||
            paintedSelected[row] != (id && id == selected)) {
            canvas.fill(0, 90 + row * 18, 704, 18,
                        id && id == selected ? 0x355d50 : 0x20262d);
            canvas.text(8, 90 + row * 18, text, 0xffffff, osgui::FontFace::Mono, 14);
            strcpy(painted[row], text);
            paintedPid[row] = id;
            paintedSelected[row] = id && id == selected;
        }
    }
    canvas.fill(0, 382, 704, 66, 0x20262d);
    if (confirming) {
        snprintf(heading, sizeof(heading),
                 "%s PID %llu UID %u %.32s? Enter=confirm Esc=cancel",
                 force ? "KILL" : "TERM", (unsigned long long)target.id, target.uid,
                 target.name);
        canvas.text(8, 386, heading, 0xffadad);
    } else {
        for (uint32_t i = 0; i < snapshot.count; ++i)
            if (snapshot.rows[i].info.id == selected) {
                char command[92];
                snprintf(command, sizeof(command), "%.88s",
                         snapshot.rows[i].info.commandLine);
                // /proc and TaskInfo may hide command lines according to OS policy.
                canvas.text(8, 386, command, 0xcccccc, osgui::FontFace::Mono, 14);
                break;
            }
    }
    if (snapshot.error)
        snprintf(message, sizeof(message), "Sampling failed: %d", snapshot.error);
    else if (snapshot.truncated)
        strcpy(message, "Process list limited to 256 rows");
    canvas.text(8, 418, message, 0xcccccc, osgui::FontFace::Ui, 14);
    paintedValid = canvas.flush(true);
    dirty = false;
}
void chooseAction(bool kill) {
    for (uint32_t i = 0; i < snapshot.count; ++i)
        if (snapshot.rows[i].info.id == selected) {
            target = snapshot.rows[i].info;
            force = kill;
            confirming = true;
            dirty = true;
            return;
        }
    strcpy(message, "Select a process first");
    dirty = true;
}
}  // namespace
int main() {
    if (!client.open("Task Manager", 704, 448)) return 1;
    std::thread thread(worker);
    while (running) {
        osgui::WindowEvent event{};
        while (client.poll(event)) {
            std::lock_guard<std::mutex> guard(lock);
            if (event.type == osgui::WindowEventType::WindowClose) {
                running = false;
                break;
            }
            if (event.type == osgui::WindowEventType::WindowVisibility)
                visible = event.x != 0;
            if (event.type == osgui::WindowEventType::MouseWheel) {
                int64_t next = int64_t(scroll) - event.delta * 3;
                scroll = next < 0 ? 0 : uint32_t(next);
                dirty = true;
            }
            if (event.type == osgui::WindowEventType::KeyDown) {
                if (confirming) {
                    if (event.keyCode == os::KeyCode::Escape) confirming = false;
                    if (event.keyCode == os::KeyCode::Enter) {
                        int status = taskmgr::terminate(target, force);
                        snprintf(message, sizeof(message), "%s: %d",
                                 status ? "Signal failed" : "Signal sent", status);
                        confirming = false;
                    }
                } else if (event.keyCode == os::KeyCode::Backspace) {
                    size_t size = strlen(filter);
                    if (size) filter[size - 1] = 0;
                } else {
                    char value = app::key(event.keyCode, event.modifiers);
                    size_t size = strlen(filter);
                    if (value && size + 1 < sizeof(filter)) {
                        filter[size] = value;
                        filter[size + 1] = 0;
                        scroll = 0;
                    }
                }
                dirty = true;
            }
            if (event.type == osgui::WindowEventType::MouseButtonDown &&
                (event.buttons & 1)) {
                if (app::inside(event, 10, 36, 90))
                    refresh = true;
                else if (app::inside(event, 110, 36, 80))
                    sort = (sort + 1) % 3;
                else if (app::inside(event, 200, 36, 112))
                    chooseAction(false);
                else if (app::inside(event, 322, 36, 112))
                    chooseAction(true);
                else if (event.y >= 90 && event.y < 378)
                    selected = paintedPid[(event.y - 90) / 18];
                dirty = true;
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
