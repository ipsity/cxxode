#include "skills/skill.hpp"

#include <doctest.h>
#include <fstream>
#include <filesystem>

using namespace lay;

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
    const auto temp = std::filesystem::temp_directory_path() / "lay-skills-test";
    std::filesystem::create_directories(temp / ".claude" / "skills");

    {
        std::ofstream f(temp / ".claude" / "skills" / "example.md");
        f << "---\n"
          << "name: example\n"
          << "description: Example skill\n"
          << "---\n"
          << "\n"
          << "Example skill body.\n";
    }

    const auto skills = load_skills(temp);
    CHECK(skills.size() == 1);
    CHECK(skills[0].name == "example");
    CHECK(skills[0].description == "Example skill");
    CHECK(skills[0].path == temp / ".claude" / "skills" / "example.md");

    std::filesystem::remove_all(temp);
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
