#pragma once
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "core/bopomofo.hpp"

namespace tsf {

enum class InputMode : uint8_t {
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

class IEngine {
public:
    virtual ~IEngine() {};
    virtual void ready() = 0;
    virtual void predict(const std::u16string &context, std::span<BopomofoPos> padding /* in out */) = 0;
    virtual void record_commit(const CommitSample& sample) = 0;
    virtual InputMode toggle_input_mode() = 0;
    virtual InputMode current_input_mode() = 0;
};

struct IEngineCtx {
    virtual ~IEngineCtx() {};
};

}  // namespace tsf
