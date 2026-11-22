#include "model/anthropic.hpp"
#include "model/messages_codec.hpp"
#include "tools/read_only.hpp"

#include <chrono>
#include <iostream>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace {

int run(std::span<const std::string> args) {
    using namespace lay;
    if (args.size() == 1 && args[0] == "codec") {
#ifdef _WIN32
        _setmode(_fileno(stdin), _O_BINARY);
#endif
        messages::Stream stream;
        for (char byte; std::cin.get(byte);) {
            if (!stream.push({&byte, 1})) {
                break;
            }
        }
        const auto failure = stream.finish([](const ModelEvent& event) {
            if (const auto* text = std::get_if<TextDelta>(&event)) {
                std::cout << text->text << '\n';
            }
            if (const auto* tool = std::get_if<ProposeTool>(&event)) {
                std::cout << tool->call.id << ' ' << tool->call.name << ' '
                          << text_arg(tool->call, "path") << '\n';
            }
            return true;
        });
        if (failure) {
            std::cout << name(failure->reason) << '\n';
            return 1;
        }
        return 0;
    }
    if (args.size() != 4 || args[0] != "session") {
        return 2;
    }
    const ReadOnlyWorkspace workspace(platform::path_from_utf8(args[2]), {"private"});
    const auto milliseconds = std::chrono::milliseconds(std::stoi(args[3]));
    const auto deadline = std::chrono::steady_clock::now() + milliseconds;
    const auto cancelled = [=] {
        return milliseconds.count() != 0 && std::chrono::steady_clock::now() >= deadline;
    };
    const auto model = messages::loopback_model("fixture-model", static_cast<std::uint16_t>(std::stoi(args[1])),
                                                cancelled);
    const auto session = run_session("Find the source location", model,
        [&](const auto& call) { return workspace.execute(call, cancelled); }, cancelled, {},
        [&](const auto& call) { return workspace.authorize(call); });
    for (const auto& round : session.rounds) {
        std::cout << round.text;
        for (const auto& record : round.tools) {
            std::cout << '[' << record.call.id << "] " << record.result.text << '\n';
        }
    }
    std::cout << name(session.stop) << " requests=" << session.requests
              << " dispatches=" << session.dispatches << ' ' << session.diagnostic << '\n';
    return session.stop == SessionStop::end_turn ? 0 : 1;
}

} // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) {
        const auto utf8 = std::filesystem::path(argv[i]).u8string();
        args.emplace_back(utf8.begin(), utf8.end());
    }
    return run(args);
}
#else
int main(int argc, char** argv) {
    return run(std::vector<std::string>(argv + 1, argv + argc));
}
#endif
