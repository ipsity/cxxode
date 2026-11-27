#include "core/agent.hpp"

#include <ctime>

namespace ode {

std::string build_system_prompt(const fs::path& root, std::string_view shell,
                                const std::vector<std::pair<fs::path, std::string>>& memory) {
    const auto t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char date[16];
    std::strftime(date, sizeof(date), "%Y-%m-%d", &tm);
    const auto root_text = root.generic_u8string();

    std::string p = R"(You are a coding assistant running inside ode (cxxode), a coding agent harness. Help users inspect files, run commands, and modify code.

<tools>
- Read: Read file contents.
- Bash: Run shell commands.
- Edit: Replace an exact string in an existing file.
- Write: Create or overwrite a file.
- After writing to a path outside the workspace, verify the exact target exists and contains the intended content before reporting success. Never claim a file was saved based only on a shell command being issued.
- Grep: Search file contents by pattern.

Additional tools may be available.
</tools>

<rules>
- Use Bash for shell operations such as listing directories. Use Glob and Grep for searches.
- Use Read to examine files instead of cat or sed.
- Use Edit for targeted changes. Combine nearby changes in one replacement when practical. Use separate calls for disjoint changes.
- Each old_string must match the file exactly and be unique unless replace_all is explicitly set. Keep matches short and include only the context needed to identify them.
- Use Write only for new files or complete rewrites.
- Be concise in your responses.
- Show file paths clearly when working with files.
</rules>

# Environment
)";
    p += "- Workspace: " + std::string(root_text.begin(), root_text.end()) + "\n";
#ifdef _WIN32
    p += "- Platform: Windows\n";
#else
    p += "- Platform: POSIX\n";
#endif
    p += "- Shell for the Bash tool: " + std::string(shell) + "\n";
    p += std::string("- Date: ") + date + "\n";
    p += std::string("- Git repository: ") + (fs::exists(root / ".git") ? "yes" : "no") + "\n";

    if (!memory.empty()) {
        p += "\n# Project and user instructions\nThese files were written by the user. Follow them; they override the "
             "defaults above.\n";
        for (const auto& [path, text] : memory) {
            const auto shown = path.generic_u8string();
            p += "\n<file path=\"" + std::string(shown.begin(), shown.end()) + "\">\n" + text + "\n</file>\n";
        }
    }
    return p;
}

} // namespace ode
