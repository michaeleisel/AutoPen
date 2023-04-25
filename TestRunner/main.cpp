// We use a test running executable, rather than a UI or unit test target, because
// `codesign` calls `exit()` upon normal exit, which messes up the test running

#include "runner.hpp"
#include "swapper.hpp"

#include <unistd.h>
#include <fstream>
#include <filesystem>
#include <sstream>
#include <vector>

using namespace std::string_literals;
namespace fs = std::filesystem;

void runCommand(const std::string &command, bool failable = false) {
    int result = system(command.c_str());
    if (!failable) {
        hardAssert(result == 0, "Command failed");
    }
}

template <typename T, typename U>
void writeFile(const T &path, const U &contents) {
    std::ofstream file(path);
    file.write(contents.data(), contents.size());
    file.close();
}

// Helper function to generate test binaries
void generateBinaries() {
    const std::string directory = "/tmp/autopen_binaries";
    const std::string commandStart = "clang -isysroot `xcode-select -p`/Platforms/iPhoneOS.platform/Developer/SDKs/iPhoneOS.sdk -target arm64-apple-ios16.1 a.c ";
    runCommand("rm -rf " + directory, true);
    runCommand("mkdir -p " + directory);

    chdir(directory.c_str());

    writeFile("a.c", std::string("int main() { return 0; }"));

    runCommand(commandStart + " -o small");

    std::string fillerPath = directory + "/filler";

    auto largeArray = std::make_unique<std::array<char, (size_t)5e7>>();
    largeArray->fill('\0');
    writeFile(fillerPath, *largeArray);
    runCommand(commandStart + " -Wl,-sectcreate,__DATA,__autopen_filler," + fillerPath + " -o large");

    /*auto largeAlignedArray = std::make_unique<std::array<char, (size_t)5e7 + 3940>>();
    largeAlignedArray->fill('\0');
    writeFile(fillerPath, *largeAlignedArray);
    runCommand(commandStart + " -Wl,-sectcreate,__TEXT,__autopen_filler," + fillerPath + " -o large_aligned");*/
}

void assertDirectoriesEqual(std::string aStr, std::string bStr) {
    auto a = fs::absolute(aStr);
    auto b = fs::absolute(bStr);
    for (const auto& dirEntry : fs::recursive_directory_iterator(a)) {
        const auto aFile = dirEntry.path();
        const auto bFile = b / fs::relative(aFile, a);
        if (!fs::exists(bFile)) {
            hardAssert(fs::exists(bFile), "File doesn't exist: " + bFile.string());
        }
        hardAssert(contentsOfFile(aFile) == contentsOfFile(bFile) || aFile == "/Users/meisel/1.app/_CodeSignature" || aFile == "/Users/meisel/1.app/iphoneo", "Files mismatch for " + aFile.string());
    }

    for (const auto& dirEntry : fs::recursive_directory_iterator(b)) {
        const auto bFile = dirEntry.path();
        const auto aFile = a / fs::relative(bFile, b);
        hardAssert(fs::exists(aFile) || aFile == "/Users/meisel/1.app/embedded.mobileprovision" || aFile == "/Users/meisel/1.app/_CodeSignature/CodeResources" || aFile == "/Users/meisel/1.app/_CodeSignature", "File doesn't exist: " + aFile.string());
    }
}

void runTests(const char *bundlePath, const char *autoPenPath, void (*testAssert)(bool, const char *)) {
    for (const auto &binary : {"large", "small"}) {
        chdir(bundlePath); // Just a directory to be in while we delete the test directory
        std::string bundle(bundlePath);

        std::string directory = "/tmp/autopen_tests";
        runCommand("rm -rf " + directory, true);
        runCommand("mkdir -p " + directory);
        chdir(directory.c_str());
        runCommand("unzip " + bundle + "/Signal.app.zip");

        runCommand("unzip " + bundle + "/binaries.zip");
        const std::string appPath = "Signal.app";
        const std::string entitlementsPath = bundle + "/entitlements.app.xcent";
        const std::string signalBinary = "Signal.app/Signal";
        runCommand("cp "s + binary + " " + signalBinary);
        // std::vector<const char *> args {autoPenPath, "--generate-entitlement-der", "-s", "-", appPath.c_str(), "--digest-algorithm=sha1", "--entitlements", entitlementsPath.c_str(), NULL};
        // int result = run(int(args.size() - 1), args.data(), NULL, NULL);
        runCommand(std::string(autoPenPath) + " --generate-entitlement-der -s - " + appPath +  " --digest-algorithm=sha1 --entitlements " + entitlementsPath);
        testAssert(contentsOfFile(signalBinary) == contentsOfFile(std::string(binary) + "_good"), "Binary mismatch");
        // hardAssert(contentsOfFile("embedded.mobileprovision") == contentsOfFile("Signal.app/embedded.mobileprovision"), "Provision mismatch");
        testAssert(contentsOfFile(bundle + "/CodeResources") == contentsOfFile("Signal.app/_CodeSignature/CodeResources"), "Resources mismatch");
        /*} catch (std::runtime_error error) {
            testAssert(false, ("Error occurred: "s + error.what()).c_str());
        }*/
    }
    // runCommand("unzip
    // runCommand("/usr/bin/codesign " +
    // Small binary
    // Binary not divisible by 4096
    // Big binary
    // Binary with diff CFBundleExecutable
    // Error case
}

void testAssert(bool condition, const char *message) {
    hardAssert(condition, message);
}

int main(int argc, char *argv[]) {
    if (argc != 3) {
        std::cout << "Usage: ./Tests <path to AutoPen binary> <path to test bundle>\n";
        return 1;
    }
    runTests(argv[2], argv[1], testAssert);
    return 0;
}
