#include "runner.hpp"
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <sstream>

// Because we use dyld interposing, there must exist a dylib that does it, and it's easier and more testable
// to just throw all the code into that dylib and make this a shell
int main(int argc, const char *argv[], const char *envp[], const char *apple[]) {
    return run(argc, argv, envp, apple);
}
