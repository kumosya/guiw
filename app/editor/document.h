#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>
namespace editor {
constexpr size_t fileLimit = 1024 * 1024;
class Document {
    struct State {
        std::vector<char> data;
        size_t cursor = 0, anchor = 0;
        uint64_t revision = 0;
    } state_;
    std::vector<State> undo_, redo_;
    uint64_t sequence_ = 0, saved_ = 0;
    size_t historyBytes_ = 0;
    bool bom_ = false, crlf_ = false;
    void checkpoint();
    void trimHistory();

   public:
    Document();
    bool load(const std::vector<char>& data);
    bool replace(const char* data, size_t length);
    bool backspace();
    bool erase();
    bool undo();
    bool redo();
    const char* text() const { return state_.data.data(); }
    size_t size() const { return state_.data.size() - 1; }
    size_t cursor() const { return state_.cursor; }
    size_t anchor() const { return state_.anchor; }
    void position(size_t position, bool extend = false);
    void move(int direction, bool extend = false);
    size_t lineStart(size_t position) const;
    size_t lineEnd(size_t position) const;
    size_t next(size_t position) const;
    size_t previous(size_t position) const;
    size_t find(const char* query, size_t from) const;
    void selectAll() {
        state_.anchor = 0;
        state_.cursor = size();
    }
    bool modified() const { return state_.revision != saved_; }
    uint64_t revision() const { return state_.revision; }
    void saved(uint64_t revision) { saved_ = revision; }
    const char* newline() const { return crlf_ ? "\r\n" : "\n"; }
    std::vector<char> serialize() const;
};
}  // namespace editor
