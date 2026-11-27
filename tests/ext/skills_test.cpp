#include "ext/discovery.hpp"
#include "ext/skills.hpp"

#include <doctest.h>
#include <fstream>
#include <filesystem>
#include <algorithm>

using namespace ode;

TEST_CASE("parse_frontmatter extracts YAML metadata") {
    const auto text = R"(---
name: test-skill
description: A test skill
---

This is the body content.
More content here.
)";
    auto [meta, body] = parse_frontmatter(text);
    CHECK(meta["name"] == "test-skill");
    CHECK(meta["description"] == "A test skill");
    CHECK(body == "\nThis is the body content.\nMore content here.\n");
}

TEST_CASE("parse_frontmatter handles missing frontmatter") {
    const auto text = "Just plain text, no frontmatter.";
    auto [meta, body] = parse_frontmatter(text);
    CHECK(meta.empty());
    CHECK(body == text);
}

TEST_CASE("parse_frontmatter handles CRLF line endings") {
    const auto text = "---\r\nname: windows-skill\r\ndescription: Windows test\r\n---\r\n\r\nBody content\r\n";
    auto [meta, body] = parse_frontmatter(text);
    CHECK(meta["name"] == "windows-skill");
    CHECK(meta["description"] == "Windows test");
    CHECK(body.find("Body content") != std::string::npos);
}

TEST_CASE("load_skills finds and parses skill files") {
    const auto temp = std::filesystem::temp_directory_path() / "ode-skills-test";
    const auto path = temp / ".claude" / "skills" / "example" / "SKILL.md";
    std::filesystem::create_directories(path.parent_path());

    {
        std::ofstream f(path);
        f << "---\n"
          << "name: example\n"
          << "description: Example skill\n"
          << "---\n"
          << "\n"
          << "Example skill body.\n";
    }

    const auto skills = load_skills(temp);
    const auto loaded = std::ranges::find(skills, path, &skill::path);
    REQUIRE(loaded != skills.end());
    CHECK(loaded->name == "example");
    CHECK(loaded->description == "Example skill");

    std::filesystem::remove_all(temp);
}

TEST_CASE("skill loading is read-only and does not require execute permission") {
    const auto temp = std::filesystem::temp_directory_path() / "ode-skill-access-test";
    const auto path = temp / ".claude" / "skills" / "example" / "SKILL.md";
    std::filesystem::create_directories(path.parent_path());
    std::ofstream(path) << "---\nname: example\ndescription: Example\n---\n\nUse Read.\n";

    const auto tools = make_skill_tools(load_skills(temp));
    REQUIRE(tools.size() == 1);
    CHECK(tools.front().access == tool_access::read);

    std::error_code ec;
    std::filesystem::remove_all(temp, ec);
}

TEST_CASE("skills_system_prompt formats skills list") {
    std::vector<skill> skills = {
        {"review", "Review code changes", {}, ""},
        {"test", "Generate tests", {}, ""}
    };

    const auto prompt = skills_system_prompt(skills);
    CHECK(prompt.find("review") != std::string::npos);
    CHECK(prompt.find("Review code changes") != std::string::npos);
    CHECK(prompt.find("test") != std::string::npos);
    CHECK(prompt.find("skill` tool") != std::string::npos);
}

TEST_CASE("skills_system_prompt returns empty for no skills") {
    const auto prompt = skills_system_prompt({});
    CHECK(prompt.empty());
}
