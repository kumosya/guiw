#include <cstdio>
#include <mutex>
#include <thread>

#include "../common/client.h"
#include "document.h"
#include "file.h"
namespace {
app::Client client;
editor::Document document;
editor::FileVersion version;
char path[512]{}, input[512]{}, message[192]{};
size_t inputLength = 0, top = 0;
uint32_t lineHeight = 1;
uint64_t rowHashes[24]{};
size_t rowStarts[24]{}, rowEnds[24]{};
int paintedMode = -1;
enum class Mode { Edit, Open, SaveAs, Find, Unsaved, Overwrite } mode = Mode::Edit;
enum class Action { None, New, Open, Close } deferred = Action::None;
bool running = true, dirty = true, selecting = false;
std::mutex ioLock;
struct Job {
    int kind = 0;
    char path[512]{};
    std::vector<char> bytes;
    editor::FileVersion expected;
    uint64_t revision = 0;
} job;
struct Completion {
    bool ready = false;
    int kind = 0;
    char path[512]{};
    editor::FileResult result;
    uint64_t revision = 0;
} completion;
bool ioRunning = true, busy = false;
int busyKind = 0;
editor::FileVersion overwriteVersion;
char overwritePath[512]{};
void setInput(Mode next) {
    mode = next;
    input[0] = 0;
    inputLength = 0;
    dirty = true;
}
void submit(int kind, const char* target, const editor::FileVersion& expected = {}) {
    std::lock_guard<std::mutex> guard(ioLock);
    if (busy) {
        strcpy(message, "File operation in progress");
        return;
    }
    job = {};
    job.kind = kind;
    strcpy(job.path, target);
    job.expected = expected;
    job.revision = document.revision();
    if (kind == 2) job.bytes = document.serialize();
    busy = true;
    busyKind = kind;
    mode = Mode::Edit;
    strcpy(message, kind == 1 ? "Opening..." : "Saving...");
    dirty = true;
}
void worker() {
    while (true) {
        Job work;
        {
            std::lock_guard<std::mutex> guard(ioLock);
            if (!ioRunning) break;
            if (job.kind) {
                work = std::move(job);
                job.kind = 0;
            }
        }
        if (!work.kind) {
            os::sleep(10);
            continue;
        }
        auto result = work.kind == 1
                          ? editor::readFile(work.path)
                          : editor::saveFile(work.path, work.bytes, work.expected);
        std::lock_guard<std::mutex> guard(ioLock);
        completion.result = std::move(result);
        completion.kind = work.kind;
        completion.revision = work.revision;
        strcpy(completion.path, work.path);
        completion.ready = true;
    }
}
void perform(Action action) {
    deferred = Action::None;
    if (action == Action::Close)
        running = false;
    else if (action == Action::Open)
        setInput(Mode::Open);
    else if (action == Action::New) {
        std::vector<char> empty;
        document.load(empty);
        version = {};
        path[0] = 0;
        top = 0;
        mode = Mode::Edit;
        dirty = true;
    }
}
void request(Action action) {
    if (busy) {
        strcpy(message, "Wait for the current file operation");
        dirty = true;
        return;
    }
    if (document.modified()) {
        deferred = action;
        mode = Mode::Unsaved;
        dirty = true;
    } else
        perform(action);
}
void save(bool saveAs = false) {
    if (busy) return;
    if (saveAs || !path[0]) {
        setInput(Mode::SaveAs);
        if (path[0]) {
            strcpy(input, path);
            inputLength = strlen(input);
        }
    } else
        submit(2, path, version);
}
void collect() {
    Completion result;
    {
        std::lock_guard<std::mutex> guard(ioLock);
        if (!completion.ready) return;
        result = std::move(completion);
        completion.ready = false;
        busy = false;
    }
    dirty = true;
    if (result.kind == 2 && (result.result.error == editor::FileError::Conflict ||
                             result.result.error == editor::FileError::Exists)) {
        overwriteVersion = std::move(result.result.version);
        strcpy(overwritePath, result.path);
        mode = Mode::Overwrite;
        strcpy(message,
               "File changed or already exists. Confirm overwrite explicitly.");
        return;
    }
    if (result.result.error != editor::FileError::None) {
        snprintf(message, sizeof(message),
                 "File operation failed (%d, errno %d); document retained",
                 int(result.result.error), result.result.systemError);
        deferred = Action::None;
        return;
    }
    if (result.kind == 1) {
        if (!result.result.version.exists) {
            strcpy(message, "File does not exist");
            return;
        }
        if (!document.load(result.result.version.bytes)) {
            strcpy(message,
                   "Rejected: invalid UTF-8, binary data, or file above 1 MiB");
            return;
        }
        version = std::move(result.result.version);
        strcpy(path, result.path);
        top = 0;
        strcpy(message, "Opened UTF-8 text");
    } else {
        version = std::move(result.result.version);
        strcpy(path, result.path);
        document.saved(result.revision);
        strcpy(message,
               "Saved (rename completed; power-loss durability is not guaranteed)");
        if (deferred != Action::None && !document.modified()) perform(deferred);
    }
}
int measure(size_t start, size_t end) {
    if (end < start) return 0;
    char text[1025];
    size_t length = end - start;
    if (length > 1024) length = 1024;
    while (length && start + length < document.size() &&
           (uint8_t(document.text()[start + length]) & 0xc0) == 0x80)
        --length;
    memcpy(text, document.text() + start, length);
    text[length] = 0;
    osgui::TextBitmap bitmap;
    return osgui::Font::render(text, length, osgui::FontFace::Mono, 16, bitmap)
               ? bitmap.advance
               : 0;
}
size_t nextLine(size_t start) {
    const size_t end = document.lineEnd(start);
    return end < document.size() ? document.next(end) : end;
}
void reveal() {
    const size_t rows = (client.height - 110) / lineHeight;
    if (document.cursor() < top) top = document.lineStart(document.cursor());
    size_t end = top;
    for (size_t i = 0; i < rows; ++i) end = nextLine(end);
    if (document.cursor() >= end && end < document.size()) {
        top = document.lineStart(document.cursor());
        for (size_t i = 1; i < rows && top; ++i)
            top = document.lineStart(document.previous(top));
    }
}
void render() {
    if (top > document.size()) top = document.lineStart(document.size());
    osgui::Canvas canvas(client.service, client.window);
    const bool full = paintedMode != int(mode);
    if (full) {
        canvas.fill(0, 0, 704, 448, 0x20262d);
        memset(rowHashes, 0, sizeof(rowHashes));
        paintedMode = int(mode);
    }
    const char* names[] = {"New", "Open", "Save", "Save As", "Find", "Undo", "Redo"};
    if (full)
        for (unsigned i = 0; i < 7; ++i)
            app::button(canvas, 8 + int(i) * 99, 6, 92, names[i]);
    char title[100];
    snprintf(title, sizeof(title), "%s%.82s", document.modified() ? "* " : "",
             path[0] ? path : "Untitled");
    canvas.fill(0, 40, 704, 22, 0x20262d);
    canvas.text(10, 43, title, 0xcccccc, osgui::FontFace::Ui, 14);
    if (mode != Mode::Edit) {
        const char* label = mode == Mode::Open     ? "Open path: "
                            : mode == Mode::SaveAs ? "Save as: "
                            : mode == Mode::Find   ? "Find: "
                            : mode == Mode::Unsaved
                                ? "Unsaved: S=save, D=discard, Esc=cancel"
                                : "Overwrite: Enter=confirm, Esc=cancel";
        canvas.fill(0, 64, 704, 32, 0x414b58);
        canvas.text(10, 70, label, 0xffffff);
        if (mode == Mode::Open || mode == Mode::SaveAs || mode == Mode::Find) {
            size_t offset = inputLength > 65 ? inputLength - 65 : 0;
            while (offset < inputLength && (uint8_t(input[offset]) & 0xc0) == 0x80)
                ++offset;
            canvas.text(110, 70, input + offset, 0xffffff, osgui::FontFace::Mono, 16);
        }
    }
    size_t start = top;
    const size_t selectionStart =
        document.cursor() < document.anchor() ? document.cursor() : document.anchor();
    const size_t selectionEnd =
        document.cursor() > document.anchor() ? document.cursor() : document.anchor();
    const int originY = mode == Mode::Edit ? 72 : 104;
    const size_t rows = (408 - originY) / lineHeight;
    bool finished = false;
    for (size_t row = 0; row < rows; ++row) {
        const size_t end = document.lineEnd(start);
        size_t visibleStart = start;
        // Keep the caret reachable on long lines; only lay out visible text.
        if (document.cursor() >= start && document.cursor() <= end) {
            visibleStart = document.cursor();
            for (unsigned i = 0; i < 90 && visibleStart > start; ++i)
                visibleStart = document.previous(visibleStart);
            while (visibleStart < document.cursor() &&
                   measure(visibleStart, document.cursor()) > 660)
                visibleStart = document.next(visibleStart);
        }
        size_t visibleEnd = visibleStart;
        unsigned characters = 0;
        while (visibleEnd < end && characters++ < 90 && visibleEnd - visibleStart < 400)
            visibleEnd = document.next(visibleEnd);
        char text[405];
        size_t length = finished ? 0 : visibleEnd - visibleStart;
        memcpy(text, document.text() + visibleStart, length);
        text[length] = 0;
        const int32_t y = originY + int32_t(row * lineHeight);
        const size_t a = selectionStart > visibleStart ? selectionStart : visibleStart,
                     b = selectionEnd < visibleEnd ? selectionEnd : visibleEnd;
        uint64_t hash = 1469598103934665603ULL;
        for (size_t i = 0; i < length; ++i) {
            hash ^= uint8_t(text[i]);
            hash *= 1099511628211ULL;
        }
        if (a < b)
            hash ^= uint64_t(a - visibleStart) * 131 + uint64_t(b - visibleStart) * 17;
        if (!finished && document.cursor() >= visibleStart &&
            document.cursor() <= visibleEnd)
            hash ^= (document.cursor() - visibleStart + 1) * 7;
        hash ^= uint64_t(finished);
        rowStarts[row] = visibleStart;
        rowEnds[row] = finished ? visibleStart : visibleEnd;
        if (rowHashes[row] != hash) {
            canvas.fill(0, y, 704, lineHeight, 0x20262d);
            if (!finished && a < b) {
                const int x = measure(visibleStart, a);
                canvas.fill(10 + x, y, measure(a, b), lineHeight, 0x355d50);
            }
            canvas.text(10, y, text, 0xffffff, osgui::FontFace::Mono, 16);
            if (!finished && document.cursor() >= visibleStart &&
                document.cursor() <= visibleEnd)
                canvas.fill(10 + measure(visibleStart, document.cursor()), y, 1,
                            lineHeight, 0x6fd0aa);
            rowHashes[row] = hash;
        }
        if (end == document.size()) finished = true;
        const size_t next = nextLine(start);
        if (next == start) finished = true;
        start = next;
    }
    canvas.fill(0, 410, 704, 38, 0x303942);
    char status[120];
    snprintf(status, sizeof(status),
             "%zu bytes | UTF-8 | %s | cursor %zu | 1 MiB limit", document.size(),
             strlen(document.newline()) == 2 ? "CRLF" : "LF", document.cursor());
    canvas.text(8, 411, status, 0xcccccc, osgui::FontFace::Ui, 14);
    canvas.text(8, 430, message, 0xffd699, osgui::FontFace::Ui, 12);
    if (!canvas.flush(true)) paintedMode = -1;
    dirty = false;
}
void copy(bool cut) {
    const size_t a =
        document.cursor() < document.anchor() ? document.cursor() : document.anchor();
    const size_t b =
        document.cursor() > document.anchor() ? document.cursor() : document.anchor();
    if (b - a > osgui::clipboardCapacity) {
        strcpy(message, "Clipboard limit: 3072 UTF-8 bytes");
        return;
    }
    if (osgui::Clipboard::set(client.service, document.text() + a, b - a) && cut)
        document.replace("", 0);
}
void paste() {
    char text[osgui::clipboardCapacity + 1];
    size_t length = 0;
    if (!osgui::Clipboard::get(client.service, text, sizeof(text), length)) return;
    if (mode == Mode::Open || mode == Mode::SaveAs || mode == Mode::Find) {
        if (inputLength + length < sizeof(input)) {
            memcpy(input + inputLength, text, length + 1);
            inputLength += length;
        }
    } else if (mode == Mode::Edit)
        document.replace(text, length);
}
void handleKey(const osgui::WindowEvent& event) {
    if (busy && busyKind == 1) return;
    const bool ctrl = event.modifiers & os::keyModifierControl,
               shift = event.modifiers & os::keyModifierShift;
    if (mode == Mode::Unsaved) {
        if (event.keyCode == os::KeyCode::S)
            save();
        else if (event.keyCode == os::KeyCode::D)
            perform(deferred);
        else if (event.keyCode == os::KeyCode::Escape) {
            deferred = Action::None;
            mode = Mode::Edit;
        }
        return;
    }
    if (mode == Mode::Overwrite) {
        if (event.keyCode == os::KeyCode::Enter)
            submit(2, overwritePath, overwriteVersion);
        else if (event.keyCode == os::KeyCode::Escape) {
            deferred = Action::None;
            mode = Mode::Edit;
        }
        return;
    }
    if (ctrl && event.keyCode == os::KeyCode::V) {
        paste();
        return;
    }
    if (mode != Mode::Edit) {
        if (event.keyCode == os::KeyCode::Escape) {
            mode = Mode::Edit;
            deferred = Action::None;
        } else if (event.keyCode == os::KeyCode::Backspace) {
            inputLength = osgui::utf8::previous(input, inputLength);
            input[inputLength] = 0;
        } else if (event.keyCode == os::KeyCode::Enter) {
            if (mode == Mode::Open && inputLength)
                submit(1, input);
            else if (mode == Mode::SaveAs && inputLength) {
                editor::FileVersion expected;
                if (!strcmp(input, path)) expected = version;
                submit(2, input, expected);
            } else if (mode == Mode::Find) {
                size_t found = document.find(input, document.cursor());
                if (found == size_t(-1)) found = document.find(input, 0);
                if (found != size_t(-1)) {
                    document.position(found);
                    document.position(found + inputLength, true);
                    reveal();
                } else
                    strcpy(message, "Text not found");
                mode = Mode::Edit;
            }
        } else {
            const char value = app::key(event.keyCode, event.modifiers);
            if (value && inputLength + 1 < sizeof(input)) {
                input[inputLength++] = value;
                input[inputLength] = 0;
            }
        }
        return;
    }
    if (ctrl) {
        switch (event.keyCode) {
            case os::KeyCode::N:
                request(Action::New);
                break;
            case os::KeyCode::O:
                request(Action::Open);
                break;
            case os::KeyCode::S:
                save(shift);
                break;
            case os::KeyCode::F:
                setInput(Mode::Find);
                break;
            case os::KeyCode::A:
                document.selectAll();
                break;
            case os::KeyCode::C:
                copy(false);
                break;
            case os::KeyCode::X:
                copy(true);
                break;
            case os::KeyCode::Z:
                if (shift)
                    document.redo();
                else
                    document.undo();
                break;
            case os::KeyCode::Y:
                document.redo();
                break;
            default:
                break;
        }
        return;
    }
    if (busy && busyKind == 1) return;
    switch (event.keyCode) {
        case os::KeyCode::Left:
            document.move(-1, shift);
            break;
        case os::KeyCode::Right:
            document.move(1, shift);
            break;
        case os::KeyCode::Home:
            document.position(document.lineStart(document.cursor()), shift);
            break;
        case os::KeyCode::End:
            document.position(document.lineEnd(document.cursor()), shift);
            break;
        case os::KeyCode::Up: {
            const size_t start = document.lineStart(document.cursor());
            if (start)
                document.position(document.lineStart(document.previous(start)), shift);
            break;
        }
        case os::KeyCode::Down:
            document.position(nextLine(document.cursor()), shift);
            break;
        case os::KeyCode::Backspace:
            document.backspace();
            break;
        case os::KeyCode::Delete:
            document.erase();
            break;
        case os::KeyCode::Enter:
            document.replace(document.newline(), strlen(document.newline()));
            break;
        case os::KeyCode::Tab:
            document.replace("\t", 1);
            break;
        default: {
            char value = app::key(event.keyCode, event.modifiers);
            if (value) document.replace(&value, 1);
            break;
        }
    }
    reveal();
}
void locate(const osgui::WindowEvent& event, bool extend) {
    const int originY = mode == Mode::Edit ? 72 : 104;
    if (event.y < originY || event.y >= 408) return;
    const uint32_t row = (event.y - originY) / lineHeight;
    if (row >= 24) return;
    const size_t start = rowStarts[row], end = rowEnds[row];
    size_t cursor = start;
    while (cursor < end) {
        const size_t next = document.next(cursor);
        if (measure(start, next) > event.x - 10) break;
        cursor = next;
    }
    document.position(cursor, extend);
}
}  // namespace
int main(int argc, char** argv) {
    if (!client.open("Text Editor", 704, 448)) return 1;
    osgui::FontMetrics metrics{};
    if (!osgui::Font::metrics(osgui::FontFace::Mono, 16, metrics)) {
        client.close();
        return 2;
    }
    lineHeight = metrics.lineHeight;
    std::thread thread(worker);
    if (argc > 1 && strlen(argv[1]) < sizeof(path)) submit(1, argv[1]);
    while (running) {
        collect();
        osgui::WindowEvent event{};
        while (client.poll(event)) {
            if (event.type == osgui::WindowEventType::WindowClose)
                request(Action::Close);
            else if (event.type == osgui::WindowEventType::KeyDown) {
                handleKey(event);
                dirty = true;
            } else if (event.type == osgui::WindowEventType::MouseWheel) {
                for (int i = 0; i < 3; ++i)
                    top = event.delta > 0 ? document.lineStart(document.previous(top))
                                          : nextLine(top);
                dirty = true;
            } else if (event.type == osgui::WindowEventType::MouseButtonDown &&
                       (event.buttons & 1)) {
                bool toolbar = false;
                if (mode == Mode::Edit)
                    for (unsigned i = 0; i < 7; ++i)
                        if (app::inside(event, 8 + int(i) * 99, 6, 92)) {
                            toolbar = true;
                            if (i == 0)
                                request(Action::New);
                            else if (i == 1)
                                request(Action::Open);
                            else if (i == 2)
                                save();
                            else if (i == 3)
                                save(true);
                            else if (i == 4)
                                setInput(Mode::Find);
                            else if (i == 5)
                                document.undo();
                            else
                                document.redo();
                        }
                if (!toolbar && mode == Mode::Edit) {
                    locate(event, event.modifiers & os::keyModifierShift);
                    selecting = true;
                }
                dirty = true;
            } else if (event.type == osgui::WindowEventType::MouseMove && selecting &&
                       (event.buttons & 1)) {
                locate(event, true);
                dirty = true;
            } else if (event.type == osgui::WindowEventType::MouseButtonUp)
                selecting = false;
        }
        if (dirty) render();
        os::sleep(16);
    }
    {
        std::lock_guard<std::mutex> guard(ioLock);
        ioRunning = false;
    }
    thread.join();
    client.close();
    return 0;
}
