#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace lay::tui {

struct Size {
    int columns;
    int rows;
    bool operator==(const Size&) const = default;
};

class Terminal {
public:
    Terminal();
    ~Terminal();
    Size size() const;
    std::optional<std::string> read(int wait_ms = 50);
    void write(std::string_view bytes) const;

private:
    struct Native;
    std::unique_ptr<Native> native_;
};

} // namespace lay::tui
