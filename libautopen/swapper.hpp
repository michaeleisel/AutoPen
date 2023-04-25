#ifndef swapper_
#define swapper_

#include <unistd.h>
#include <sys/sysctl.h>
#include <sys/stat.h>
#include <sys/fcntl.h>
#include <sys/mman.h>
#include <thread>
#include <vector>
#include <iostream>
#include <array>
#include <filesystem>
#include <fstream>
#include <CommonCrypto/CommonDigest.h>

/* The classes below are exported */
#pragma GCC visibility push(default)

namespace fs = std::filesystem;

static std::vector<char> contentsOfFile(fs::path path) {
    std::vector<char> contents;
    std::ifstream file(path);
    file.seekg(0, std::ios_base::end);
    std::streampos fileSize = file.tellg();
    contents.resize(fileSize);

    file.seekg(0, std::ios_base::beg);
    file.read(&contents[0], fileSize);
    return contents;
}

__attribute__((always_inline)) static void hardAssert(bool condition, const std::string &message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

static int fetchCoreCount() {
    unsigned int ncpus;
    int mib[2];
    size_t len = sizeof(ncpus);
    mib[0] = CTL_HW;
    mib[1] = HW_NCPU;
    if (sysctl(mib, 2, &ncpus, &len, NULL, 0) != 0) {
        ncpus = 1;
    }
    return (int)ncpus;
}

static bool sHasDoneBefore = false;

class FileReader {
    using Digest = std::array<uint8_t, 20>;
    size_t totalSize;
    std::string path;
    std::vector<Digest> digests;
    int digestIndex;
    std::unique_ptr<char, std::function<void(char*)>> buffer;

public:
    size_t position;
    int origFd;

    FileReader(int origFd, std::string path) : origFd(origFd), path(path), digestIndex(0), position(0) {
        hardAssert(!sHasDoneBefore, "Can't have multiple FileReaders");
        sHasDoneBefore = true;

        int fd = open(path.c_str(), O_RDONLY);
        fcntl(fd, F_NOCACHE, 1);
        struct stat info = {0};
        fstat(fd, &info);
        size_t fileSize = info.st_size;
        char *mmapResult = (char *)mmap(NULL, info.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
        buffer = std::unique_ptr<char, std::function<void(char*)>>(mmapResult, [fileSize, fd](char *buffer) {
            munmap(buffer, fileSize);
            close(fd);
        });

        totalSize = fileSize;
        populateDigests();
    }

    void populateDigests() {
        int coreCount = fetchCoreCount();
        std::vector<std::thread> threads;
        size_t totalReads = (totalSize + 4095) / 4096;
        digests.resize(totalReads);
        size_t readsPerThread = (totalReads + (coreCount - 1)) / coreCount;
        for (int i = 0; i < coreCount; i++) {
            threads.emplace_back([this, i, readsPerThread, totalReads]() {
                size_t start = readsPerThread * i;
                size_t end = std::min(readsPerThread * (i + 1), totalReads);
                for (size_t j = start; j < end; j++) {
                    size_t byteStart = 4096 * j;
                    size_t length = std::min(totalSize - byteStart, (size_t)4096);
                    CC_SHA1(buffer.get() + byteStart, (CC_LONG)length, digests[j].data());
                }
            });
        }
        for (auto &thread : threads) {
            thread.join();
        }
        buffer.reset(); // Don't need the huge mmap'd buffer after this point
    }

    bool copyNextDigestAndAdvance(unsigned char *dest) {
        memcpy(dest, digests[digestIndex].data(), sizeof(digests[digestIndex]));
        digestIndex++;
        return digestIndex >= digests.size();
    }

    ssize_t fakeRead(size_t size) {
        size_t toRead = std::min(size, totalSize - position);
        position += toRead;
        return toRead;
    }
};

void initState(const std::string &mainBinaryPath);

#pragma GCC visibility pop
#endif
