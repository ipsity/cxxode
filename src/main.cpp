#include "core/version.hpp"

#include <cstdio>
#include <string_view>

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--version" || arg == "-v") {
            std::printf("lay %.*s\n", static_cast<int>(lay::version().size()), lay::version().data());
            return 0;
        }
    }
    std::fputs("lay: V2 skeleton (M0). The agent loop arrives in M1.\n", stderr);
    return 0;
}
