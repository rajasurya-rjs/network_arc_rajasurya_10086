#pragma once

#include <unistd.h>

namespace calc {

// Owns a POSIX file descriptor and closes it when it goes out of scope, so
// no exit path (early return, exception, connection teardown) can leak a
// socket. Move-only, because two owners would close the same descriptor twice.
class FileDescriptor {
public:
    FileDescriptor() = default;
    explicit FileDescriptor(int fd) : fd_(fd) {}
    ~FileDescriptor() { reset(); }

    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;

    FileDescriptor(FileDescriptor&& other) noexcept : fd_(other.release()) {}
    FileDescriptor& operator=(FileDescriptor&& other) noexcept {
        if (this != &other) reset(other.release());
        return *this;
    }

    int get() const { return fd_; }
    bool valid() const { return fd_ >= 0; }

    int release() {
        const int fd = fd_;
        fd_ = -1;
        return fd;
    }

    // close() is deliberately not retried on EINTR. On Linux the descriptor
    // is already released when close() reports EINTR, so a retry could close
    // an unrelated descriptor that happened to reuse the number.
    void reset(int fd = -1) {
        if (fd_ >= 0) ::close(fd_);
        fd_ = fd;
    }

private:
    int fd_ = -1;
};

}  // namespace calc
