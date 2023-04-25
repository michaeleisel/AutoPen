#include <iostream>
#include <unistd.h>
#include <fcntl.h>
#include <unordered_set>
#include <unordered_map>

#include "swapper.hpp"

struct State {
    std::unordered_set<int> binaryFds;
    std::unordered_map<int, std::string> fdToPath;
    std::optional<FileReader> currentReader;
    std::string mainBinaryPath;
};

static State *sState;

void initState(const std::string &mainBinaryPath) {
    sState = new State;
    sState->mainBinaryPath = std::string(mainBinaryPath);
}

int my_close(int fd) {
    if (sState) {
        sState->binaryFds.erase(fd);
        sState->fdToPath.erase(fd);
    }
    return close(fd);
}

extern "C" {
int CCDigestUpdate(void * /*CCDigestRef*/ ctx, const void *data, size_t length);
int CCDigestFinal(void * /*CCDigestRef*/ ctx, uint8_t *out);
}

int my_fstat(int fildes, struct stat *buf) {
    if (sState && sState->currentReader.has_value()) {
        // codesign does a lot of slow and useless fstat calls during this signing, just make them no-ops
        return 0;
    }
    return fstat(fildes, buf);
}

int my_CCDigestFinal(void * /*CCDigestRef*/ c, uint8_t *out) {
    if (sState && sState->currentReader.has_value()) {
        bool isDone = sState->currentReader->copyNextDigestAndAdvance(const_cast<unsigned char *>(out));
        hardAssert(!isDone, "Can't advance digests past end of binary");
        return 0;
    }

    return CCDigestFinal(c, out);
}

int my_CCDigestUpdate(/*CCDigestRef*/ void *ctx, const unsigned char *data, size_t length) {
    if (sState && sState->currentReader.has_value()) {
        hardAssert(length == 4096, "Invalid length value");
        // Skip hashing, just return from the cache during CCDigestFinal
        return 0;
    }

    return CCDigestUpdate(ctx, data, length);
}

ssize_t my_read(int fd, void *buf, size_t nbyte) {
    if (nbyte == 4096 && sState && sState->binaryFds.contains(fd) && !sState->currentReader.has_value()) {
        sState->currentReader = FileReader(fd, sState->fdToPath[fd]);
    }
    if (sState && sState->currentReader.has_value()) {
        if (fd == sState->currentReader->origFd) {
            if (nbyte == 4096) {
                return sState->currentReader->fakeRead(nbyte);
            } else {
                // Originally, I thought this happens when it's trying to read that last little bit,
                // i.e. (amount to sign) % 4096. However, it always seems to be 176 bytes, so ¯\_(ツ)_/¯
                // TODO: Figure out why this amount doesn't equal 4096
                lseek(fd, sState->currentReader->position, SEEK_SET);
                sState->currentReader.reset();
            }
        } else {
            sState->currentReader.reset();
        }
    }
    return read(fd, buf, nbyte);
}

int my_open(const char *path, int oflag, ...) {
    va_list ap = {0};
    mode_t mode = 0;

    int fd = 0;
    int openErrno = 0;
    if ((oflag & O_CREAT) != 0) {
        // mode only applies to O_CREAT
        va_start(ap, oflag);
        mode = va_arg(ap, int);
        va_end(ap);
        
        fd = open(path, oflag, mode);
        openErrno = errno;
    } else {
        fd = open(path, oflag);
        openErrno = errno;
    }

    if (sState && sState->mainBinaryPath + ".cstemp" == path) {
        // close(fd);
        sState->binaryFds.emplace(fd);
    }
    if (sState) {
        sState->fdToPath[fd] = path;
    }

    errno = openErrno;
    return fd;
}

// Fishhook doesn't work when the functions are called from within codesign, so use interposing instead
#define DYLD_INTERPOSE(_replacment,_replacee) \
   __attribute__((used)) static struct{ const void* replacment; const void* replacee; } _interpose_##_replacee \
            __attribute__ ((section ("__DATA,__interpose"))) = { (const void*)(unsigned long)&_replacment, (const void*)(unsigned long)&_replacee };

DYLD_INTERPOSE(my_open, open);
DYLD_INTERPOSE(my_close, close);
DYLD_INTERPOSE(my_read, read);
DYLD_INTERPOSE(my_fstat, fstat);
DYLD_INTERPOSE(my_CCDigestUpdate, CCDigestUpdate);
DYLD_INTERPOSE(my_CCDigestFinal, CCDigestFinal);
