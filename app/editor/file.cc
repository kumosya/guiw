#include "file.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
namespace editor {
FileResult readFile(const char* path) {
    FileResult result;
    const int descriptor = open(path, O_RDONLY);
    if (descriptor < 0) {
        if (errno != ENOENT) {
            result.error = FileError::Io;
            result.systemError = errno;
        }
        return result;
    }
    struct stat status{};
    if (fstat(descriptor, &status) || !S_ISREG(status.st_mode)) {
        result.error = FileError::Io;
        result.systemError = errno ? errno : EINVAL;
        close(descriptor);
        return result;
    }
    if (status.st_size < 0 || uint64_t(status.st_size) > fileLimit) {
        result.error = FileError::TooLarge;
        close(descriptor);
        return result;
    }
    result.version.exists = true;
    result.version.inode = status.st_ino;
    result.version.modified = status.st_mtime;
    result.version.bytes.resize(status.st_size);
    if (result.version.bytes.size() != size_t(status.st_size)) {
        result.error = FileError::Io;
        result.systemError = ENOMEM;
        close(descriptor);
        return result;
    }
    size_t offset = 0;
    while (offset < result.version.bytes.size()) {
        ssize_t count = read(descriptor, result.version.bytes.data() + offset,
                             result.version.bytes.size() - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) {
            result.error = FileError::Io;
            result.systemError = count < 0 ? errno : EIO;
            break;
        }
        offset += count;
    }
    char extra;
    if (result.error == FileError::None && read(descriptor, &extra, 1) != 0) {
        result.error = FileError::Conflict;
    }
    if (close(descriptor) && result.error == FileError::None) {
        result.error = FileError::Io;
        result.systemError = errno;
    }
    return result;
}
FileResult saveFile(const char* path, const std::vector<char>& bytes,
                    const FileVersion& expected) {
    if (bytes.size() > fileLimit) {
        FileResult result;
        result.error = FileError::TooLarge;
        return result;
    }
    auto current = readFile(path);
    if (current.error != FileError::None) return current;
    bool equal = current.version.exists == expected.exists;
    if (equal && expected.exists)
        equal = current.version.inode == expected.inode &&
                current.version.modified == expected.modified &&
                current.version.bytes.size() == expected.bytes.size() &&
                (!expected.bytes.size() ||
                 !memcmp(current.version.bytes.data(), expected.bytes.data(),
                         expected.bytes.size()));
    if (!equal) {
        current.error = expected.exists ? FileError::Conflict : FileError::Exists;
        return current;
    }
    char temporary[640];
    int descriptor = -1;
    static uint32_t sequence = 0;
    for (unsigned attempt = 0; attempt < 16; ++attempt) {
        const int length = snprintf(temporary, sizeof(temporary), "%s.guiw-%llu-%u.tmp",
                                    path, (unsigned long long)getpid(), ++sequence);
        if (length < 0 || size_t(length) >= sizeof(temporary)) {
            current.error = FileError::Io;
            current.systemError = ENAMETOOLONG;
            return current;
        }
        descriptor = open(temporary, O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (descriptor >= 0 || errno != EEXIST) break;
    }
    auto fail = [&](int error) {
        if (descriptor >= 0) close(descriptor);
        unlink(temporary);
        current.error = FileError::Io;
        current.systemError = error;
        return current;
    };
    if (descriptor < 0) {
        current.error = FileError::Io;
        current.systemError = errno;
        return current;
    }
    if (expected.exists) {
        struct stat status{};
        if (stat(path, &status) || fchmod(descriptor, status.st_mode & 0777))
            return fail(errno);
    }
    size_t offset = 0;
    while (offset < bytes.size()) {
        const ssize_t count =
            write(descriptor, bytes.data() + offset, bytes.size() - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return fail(count < 0 ? errno : EIO);
        offset += count;
    }
    if (close(descriptor)) {
        descriptor = -1;
        return fail(errno);
    }
    descriptor = -1;
    // Recheck after staging. OSInfo currently has no compare-and-replace call;
    // a concurrent change between this check and rename remains a narrow race.
    const auto recheck = readFile(path);
    if (recheck.error != FileError::None ||
        recheck.version.exists != current.version.exists ||
        (current.version.exists &&
         (recheck.version.inode != current.version.inode ||
          recheck.version.modified != current.version.modified ||
          recheck.version.bytes.size() != current.version.bytes.size() ||
          (current.version.bytes.size() &&
           memcmp(recheck.version.bytes.data(), current.version.bytes.data(),
                  current.version.bytes.size()))))) {
        unlink(temporary);
        current.error = FileError::Conflict;
        return current;
    }
    if (rename(temporary, path)) return fail(errno);
    current = readFile(path);
    return current;
}
}  // namespace editor
