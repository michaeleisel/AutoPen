#include <cstdarg>
#include <iostream>
#include <string>
#include <iterator>
#include <array>
#include <algorithm>
#include <optional>

#include <mach-o/swap.h>
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/errno.h>
#include <unistd.h>

#include <CoreFoundation/CoreFoundation.h>

#include "swapper.hpp"
#include "runner.hpp"

using namespace std::string_literals;

using EntryPointFunc = int (*)(int argc, const char *argv[], const char *envp[], const char *apple[]);

uint64_t find_lc_main(const char *filename, cpu_type_t target_arch) {
    int fd = open(filename, O_RDONLY);
    hardAssert(fd >= 0, "open failed");

    struct stat st;
    int statResult = fstat(fd, &st) < 0;
    hardAssert(statResult == 0, "open failed");

    void *buffer = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    hardAssert(buffer != MAP_FAILED, "mmap failed");

    struct fat_header *header = (struct fat_header *)buffer;
    hardAssert(OSSwapBigToHostInt32(header->magic) == FAT_MAGIC, "Not a fat Mach-O binary");

    uint64_t lc_main_value = 0;
    struct fat_arch *arch = (struct fat_arch *)(header + 1);

    for (uint32_t i = 0; i < OSSwapBigToHostInt32(header->nfat_arch); ++i, ++arch) {
        if (OSSwapBigToHostInt32(arch->cputype) == target_arch) {
            struct mach_header_64 *mh = (struct mach_header_64 *)((char *)buffer + OSSwapBigToHostInt32(arch->offset));
            hardAssert(mh->magic == MH_MAGIC_64, "Not a 64-bit Mach-O binary");

            struct load_command *lc = (struct load_command *)(mh + 1);

            for (uint32_t j = 0; j < mh->ncmds; ++j, lc = (struct load_command *)((char *)lc + lc->cmdsize)) {
                if (lc->cmd == LC_MAIN) {
                    struct entry_point_command *epc = (struct entry_point_command *)lc;
                    lc_main_value = epc->entryoff;
                    break;
                }
            }

            break;
        }
    }

    munmap(buffer, st.st_size);
    close(fd);
    return lc_main_value;
}

static double sStart;

double relativeTime() {
    uint64_t nsecs = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    return nsecs / 1e9;
}

std::string executableForBundle(std::string bundlePathStr) {
    CFStringRef bundlePath = CFStringCreateWithCString(NULL, bundlePathStr.c_str(), kCFStringEncodingUTF8);
    CFURLRef bundleUrl = CFURLCreateWithString(kCFAllocatorDefault, bundlePath, NULL);
    CFBundleRef mainBundle = CFBundleCreate(kCFAllocatorDefault, bundleUrl);
    CFDictionaryRef dictionary = CFBundleGetInfoDictionary(mainBundle);
    CFStringRef executable = (CFStringRef)CFDictionaryGetValue(dictionary, CFSTR("CFBundleExecutable"));
    CFIndex maxSize = CFStringGetMaximumSizeForEncoding(CFStringGetLength(executable), kCFStringEncodingUTF8);
    std::string executableStr;
    executableStr.reserve(maxSize);
    CFStringGetCString(executable, executableStr.data(), executableStr.capacity() + 1, kCFStringEncodingUTF8);
    return executableStr;
}

bool startsWith(const std::string &haystack, const std::string &needle) {
    return haystack.rfind(needle, 0) == 0;
}

static const std::vector<const std::string> kArgsThatTakeFlags = {"-a", "--architecture", "--bundle-version", "-D", "--detached", "-i", "--identifier", "-o", "--options", "-P", "--pagesize", "-r", "--requirements", "-R", "--test-requirement", "-s", "--sign", "--entitlements", "--extract-certificates", "--file-list", "--keychain", "--prefix", "--strict", "--timestamp", "--runtime-version"};

int run(int argc, const char *argv[], const char *envp[], const char *apple[]) {
    sStart = relativeTime();
    const char *appPathArg = NULL;
    for (int i = 1; i < argc; i++) {
        const auto arg = std::string(argv[i]);
        const auto argIter = std::find(kArgsThatTakeFlags.begin(), kArgsThatTakeFlags.end(), arg);
        bool takesArg = argIter != kArgsThatTakeFlags.end();
        if (takesArg) {
            i++; // Skip over the argument passed for this one
        } else if (startsWith(arg, "-")) {
            // It's a flag like --digest-algorithm=sha1 or -d, don't do anything
        } else {
            // It looks like it's the binary
            hardAssert(!appPathArg, "Multiple candidate binary names found, "s + (appPathArg ?: "") + " and " + argv[i]);
            appPathArg = argv[i];
        }
    }
    hardAssert(appPathArg, "App path not found");
    const auto realPath = std::unique_ptr<char, std::function<void(char *)>>(realpath(appPathArg, NULL), [](char *ptr) {
        free(ptr);
    });
    std::string appPath(realPath.get());
    hardAssert(appPath.rfind(".app") == appPath.size() - 4, "App argument not found");
    const auto binaryName = appPath.substr(appPath.rfind("/") + 1, appPath.size() - appPath.rfind("/") - 5);

    initState(appPath + "/" + binaryName);
    const char *binaryPath = "/usr/bin/codesign";
    argv[0] = binaryPath;

    bool usesSha1 = false;
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--digest-algorithm=sha1") == 0) {
            usesSha1 = true;
            break;
        }
    }
    hardAssert(usesSha1, "Must use SHA-1 for hashing. Please include the --digest-algorithm=sha1 flag");

    uintptr_t entry_offset = find_lc_main(binaryPath, CPU_TYPE_ARM64);
    hardAssert(entry_offset != 0, "Failed to find LC_MAIN entry point");

    void *handle = dlopen(binaryPath, RTLD_LOCAL | RTLD_LAZY);
    hardAssert(!!handle, "dlopen failed");

    const struct mach_header *header = NULL;
    for (int i = 0; i < _dyld_image_count(); i++) {
        if (strcmp(_dyld_get_image_name(i), binaryPath) == 0) {
            header = _dyld_get_image_header(i);
            break;
        }
    }
    hardAssert(header, "Must be able to find dylib image");
    EntryPointFunc entry_point = (EntryPointFunc)((uintptr_t)header + entry_offset);
    int returnCode = entry_point(argc, argv, envp, apple);

    dlclose(handle);
    return returnCode;
}

/*__attribute__((destructor)) void printTime() {
    double end = relativeTime();
    printf("Took %lf\n", end - sStart);
}*/

