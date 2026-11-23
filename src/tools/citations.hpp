#pragma once

#include "core/session.hpp"

namespace lay {

struct SourceCitation {
    std::string path;
    std::string line;
    std::string_view version;
    std::string_view observed_utc;
    std::size_t task;
    std::size_t round;
    bool historical;
    bool inline_form;
};

std::vector<SourceCitation> source_citations(const Session& session, std::size_t answer_round);

} // namespace lay
