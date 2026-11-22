#include "model/scripted_model.hpp"

#include <atomic>
#include <csignal>
#include <iostream>
#include <string_view>
#include <utility>

namespace {

std::atomic_flag interrupted;

void on_interrupt(int) {
    interrupted.test_and_set(std::memory_order_relaxed);
}

} // namespace

int main(int argc, char** argv) {
    auto script = lay::Script::normal;
    if (argc == 2 && std::string_view(argv[1]) == "--truncated") {
        script = lay::Script::truncated;
    } else if (argc != 1) {
        std::cerr << "Usage: lay-code [--truncated]\n";
        return 2;
    }

    std::cout << "lay-code> " << std::flush;
    std::string question;
    if (!std::getline(std::cin, question) || question.empty()) {
        std::cerr << "Enter a question to start the offline session.\n";
        return 2;
    }

    std::signal(SIGINT, on_interrupt);
    const auto session = lay::run_session(std::move(question), lay::scripted_model(script),
        [](const lay::ReadCall& call) -> lay::ReadResult {
            if (call.path == lay::fixture_path) {
                return {lay::ReadStatus::ok, "empty_config = allow"};
            }
            return {lay::ReadStatus::failed, "fixture path not found"};
        }, [] { return interrupted.test(std::memory_order_relaxed); });

    for (const auto& round : session.rounds) {
        for (const auto& record : round.reads) {
            if (record.dispatched) {
                std::cout << "[fixture read] " << record.call.path << '\n';
            }
        }
        if (!round.text.empty()) {
            std::cout << round.text << '\n';
        }
    }
    std::cout << "[session] " << lay::name(session.stop) << " requests=" << session.requests
              << " dispatches=" << session.dispatches << '\n';
    if (session.stop == lay::SessionStop::cancelled) {
        return 130;
    }
    return session.stop == lay::SessionStop::end_turn ? 0 : 1;
}
