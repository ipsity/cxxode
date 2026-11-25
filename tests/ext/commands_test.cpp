#include "ext/commands.hpp"
#include "ext/skills.hpp"

#include <doctest.h>
#include <fstream>

using namespace ode;

TEST_CASE("expand_variables replaces variable references") {
    std::map<std::string, std::string> vars;
    vars["ARGUMENTS"] = "test args";
    vars["WORKSPACE"] = "/home/user/project";

    CHECK(expand_variables("No variables here", vars) == "No variables here");
    CHECK(expand_variables("Args: $ARGUMENTS", vars) == "Args: test args");
    CHECK(expand_variables("$WORKSPACE/file.txt", vars) == "/home/user/project/file.txt");
    CHECK(expand_variables("$ARGUMENTS and $WORKSPACE", vars) == "test args and /home/user/project");
}

TEST_CASE("expand_variables handles unknown variables") {
    std::map<std::string, std::string> vars;
    vars["KNOWN"] = "value";

    CHECK(expand_variables("$UNKNOWN", vars) == "$UNKNOWN");
    CHECK(expand_variables("$KNOWN $UNKNOWN", vars) == "value $UNKNOWN");
}

TEST_CASE("expand_variables handles edge cases") {
    std::map<std::string, std::string> vars;
    vars["VAR"] = "replacement";

    CHECK(expand_variables("$", vars) == "$");
    CHECK(expand_variables("$$VAR", vars) == "$replacement");
    CHECK(expand_variables("text$", vars) == "text$");
}

TEST_CASE("load_commands finds and parses command files") {
    const auto temp = std::filesystem::temp_directory_path() / "ode-commands-test";
    std::filesystem::create_directories(temp / ".claude" / "commands");

    {
        std::ofstream f(temp / ".claude" / "commands" / "review.md");
        f << "---\n"
          << "name: review\n"
          << "description: Review the current changes\n"
          << "---\n"
          << "\n"
          << "Please review the git diff in $WORKSPACE.\n"
          << "Arguments: $ARGUMENTS\n";
    }

    const auto commands = load_commands(temp);
    CHECK(commands.size() == 1);
    CHECK(commands[0].name == "review");
    CHECK(commands[0].description == "Review the current changes");

    std::filesystem::remove_all(temp);
}

TEST_CASE("user_command uses stem as name if name not in frontmatter") {
    const auto temp = std::filesystem::temp_directory_path() / "ode-commands-stem-test";
    std::filesystem::create_directories(temp / ".claude" / "commands");

    {
        std::ofstream f(temp / ".claude" / "commands" / "my-command.md");
        f << "---\n"
          << "description: A command without explicit name\n"
          << "---\n"
          << "Body here.\n";
    }

    const auto commands = load_commands(temp);
    CHECK(commands.size() == 1);
    CHECK(commands[0].name == "my-command");

    std::filesystem::remove_all(temp);
}
