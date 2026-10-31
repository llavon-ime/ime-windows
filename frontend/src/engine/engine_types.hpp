#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace tsf {

enum class InputMode : std::uint8_t {
    Chinese = 0,
    English = 1,
};

struct CommitInputEntry {
    std::u16string reading;
    std::u16string output;
    bool manually_selected = false;
};

struct CommitSample {
    std::u16string context;
    std::u16string answer;
    std::vector<CommitInputEntry> input;
};

}  // namespace tsf
