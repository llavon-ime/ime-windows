#pragma once
#include <cstdint>
#include <span>
#include <string>

#include "core/bopomofo.hpp"

namespace tsf {

enum class InputMode : uint8_t {
    Chinese = 0,
    English = 1,
};

class IEngine {
public:
    virtual ~IEngine() {};
    virtual void ready() = 0;
    virtual void predict(const std::u16string &context, std::span<BopomofoPos> padding /* in out */) = 0;
    virtual InputMode toggle_input_mode() = 0;
    virtual InputMode current_input_mode() = 0;
};

struct IEngineCtx {
    virtual ~IEngineCtx() {};
};

}  // namespace tsf
