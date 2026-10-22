#include "document.h"

#include <cstring>

#include "osgui/font.h"
namespace editor {
Document::Document() { state_.data.push_back(0); }
bool Document::load(const std::vector<char>& bytes) {
    if (bytes.size() > fileLimit) return false;
    size_t start = 0;
    const bool bom = bytes.size() >= 3 && uint8_t(bytes[0]) == 0xef &&
                     uint8_t(bytes[1]) == 0xbb && uint8_t(bytes[2]) == 0xbf;
    if (bom) start = 3;
    const char* input = bytes.empty() ? "" : bytes.data();
    if (!osgui::utf8::valid(input + start, bytes.size() - start)) return false;
    for (size_t i = start; i < bytes.size(); ++i)
        if (uint8_t(bytes[i]) < 32 && bytes[i] != '\r' && bytes[i] != '\n' &&
            bytes[i] != '\t')
            return false;
    State replacement;
    replacement.data.resize(bytes.size() - start + 1);
    if (replacement.data.size() != bytes.size() - start + 1) return false;
    if (bytes.size() > start)
        memcpy(replacement.data.data(), bytes.data() + start, bytes.size() - start);
    replacement.data.back() = 0;
    replacement.revision = ++sequence_;
    bom_ = bom;
    crlf_ = false;
    for (size_t i = start; i < bytes.size(); ++i)
        if (bytes[i] == '\n') {
            crlf_ = i && bytes[i - 1] == '\r';
            break;
        }
    state_ = replacement;
    saved_ = state_.revision;
    undo_.clear();
    redo_.clear();
    historyBytes_ = 0;
    return true;
}
void Document::trimHistory() {
    while (undo_.size() > 64 || historyBytes_ > 8 * fileLimit) {
        if (undo_.empty()) break;
        historyBytes_ -= undo_[0].data.size();
        for (size_t i = 1; i < undo_.size(); ++i) undo_[i - 1] = std::move(undo_[i]);
        undo_.pop_back();
    }
}
void Document::checkpoint() {
    redo_.clear();
    undo_.push_back(state_);
    historyBytes_ = 0;
    for (const auto& state : undo_) historyBytes_ += state.data.size();
    trimHistory();
}
bool Document::replace(const char* data, size_t length) {
    const size_t begin = state_.cursor < state_.anchor ? state_.cursor : state_.anchor;
    const size_t end = state_.cursor > state_.anchor ? state_.cursor : state_.anchor;
    if (length > fileLimit || size() - (end - begin) > fileLimit - length ||
        !osgui::utf8::valid(data, length))
        return false;
    State replacement;
    replacement.data.resize(size() - (end - begin) + length + 1);
    if (replacement.data.size() != size() - (end - begin) + length + 1) return false;
    memcpy(replacement.data.data(), text(), begin);
    if (length) memcpy(replacement.data.data() + begin, data, length);
    memcpy(replacement.data.data() + begin + length, text() + end, size() - end + 1);
    replacement.cursor = replacement.anchor = begin + length;
    replacement.revision = ++sequence_;
    checkpoint();
    state_ = std::move(replacement);
    return true;
}
size_t Document::next(size_t position) const {
    if (position >= size()) return size();
    if (text()[position] == '\r' && position + 1 < size() &&
        text()[position + 1] == '\n')
        return position + 2;
    uint32_t c;
    size_t offset = position;
    return osgui::utf8::next(text(), size(), offset, c) ? offset : position + 1;
}
size_t Document::previous(size_t position) const {
    if (position > size()) position = size();
    if (position >= 2 && text()[position - 2] == '\r' && text()[position - 1] == '\n')
        return position - 2;
    return osgui::utf8::previous(text(), position);
}
void Document::position(size_t position, bool extend) {
    if (position > size()) position = size();
    while (position && position < size() && (uint8_t(text()[position]) & 0xc0) == 0x80)
        --position;
    if (position && position < size() && text()[position - 1] == '\r' &&
        text()[position] == '\n')
        --position;
    state_.cursor = position;
    if (!extend) state_.anchor = position;
}
void Document::move(int direction, bool extend) {
    position(direction < 0 ? previous(cursor()) : next(cursor()), extend);
}
bool Document::backspace() {
    if (cursor() == anchor()) {
        if (!cursor()) return false;
        state_.anchor = previous(cursor());
    }
    return replace("", 0);
}
bool Document::erase() {
    if (cursor() == anchor()) {
        if (cursor() == size()) return false;
        state_.anchor = next(cursor());
    }
    return replace("", 0);
}
bool Document::undo() {
    if (undo_.empty()) return false;
    redo_.push_back(state_);
    state_ = std::move(undo_.back());
    undo_.pop_back();
    return true;
}
bool Document::redo() {
    if (redo_.empty()) return false;
    undo_.push_back(state_);
    state_ = std::move(redo_.back());
    redo_.pop_back();
    return true;
}
size_t Document::lineStart(size_t position) const {
    if (position > size()) position = size();
    while (position && text()[position - 1] != '\n') --position;
    return position;
}
size_t Document::lineEnd(size_t position) const {
    if (position > size()) position = size();
    while (position < size() && text()[position] != '\n' && text()[position] != '\r')
        ++position;
    return position;
}
size_t Document::find(const char* query, size_t from) const {
    if (!query || !*query || from > size()) return size_t(-1);
    const char* found = strstr(text() + from, query);
    return found ? size_t(found - text()) : size_t(-1);
}
std::vector<char> Document::serialize() const {
    std::vector<char> out(size() + (bom_ ? 3 : 0));
    size_t offset = 0;
    if (bom_) {
        out[0] = char(0xef);
        out[1] = char(0xbb);
        out[2] = char(0xbf);
        offset = 3;
    }
    if (size()) memcpy(out.data() + offset, text(), size());
    return out;
}
}  // namespace editor
