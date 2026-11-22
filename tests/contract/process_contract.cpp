#include "platform/process.hpp"

#include <nlohmann/json.hpp>

#include <iostream>

namespace {

int run(const lay::TextList& args) {
    using namespace lay;
    using namespace std::chrono;
    if (args.size() < 4) {
        return 2;
    }
    const auto directory = platform::Node::root(platform::path_from_utf8(args[0]));
    const auto cancellation = milliseconds(std::stoi(args[3]));
    const auto after = steady_clock::now() + cancellation;
    const auto stop = [&] { return cancellation.count() != 0 && steady_clock::now() >= after; };
    const auto result = platform::run_process({
        platform::path_from_utf8(args[1]), directory, TextList(args.begin() + 4, args.end()),
        platform::child_environment(), milliseconds(std::stoi(args[2])),
    }, stop);
    const auto capture = [](const CapturedOutput& stream) -> nlohmann::json {
        return {{"head", stream.head}, {"tail", stream.tail}, {"total", stream.total_bytes},
                {"omitted", stream.omitted_bytes}};
    };
    std::cout << nlohmann::json{
        {"status", describe(result.status, result.code)}, {"code", result.code},
        {"descendants_terminated", result.descendants_terminated}, {"diagnostic", result.diagnostic},
        {"elapsed_ms", result.elapsed_ms}, {"stdout", capture(result.stdout_text)},
        {"stderr", capture(result.stderr_text)},
    }.dump() << '\n';
    return 0;
}

} // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    lay::TextList args;
    for (int i = 1; i < argc; ++i) {
        const auto utf8 = std::filesystem::path(argv[i]).u8string();
        args.emplace_back(utf8.begin(), utf8.end());
    }
    return run(args);
}
#else
int main(int argc, char** argv) {
    return run(lay::TextList(argv + 1, argv + argc));
}
#endif
