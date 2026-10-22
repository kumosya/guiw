#pragma once
#include "document.h"
namespace editor {
struct FileVersion {
    bool exists = false;
    uint64_t inode = 0, modified = 0;
    std::vector<char> bytes;
};
enum class FileError { None, Io, TooLarge, Conflict, Exists };
struct FileResult {
    FileError error = FileError::None;
    int systemError = 0;
    FileVersion version;
};
FileResult readFile(const char* path);
FileResult saveFile(const char* path, const std::vector<char>& bytes,
                    const FileVersion& expected);
}  // namespace editor
