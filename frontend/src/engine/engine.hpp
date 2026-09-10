#pragma once

#include <memory>

#include "impl/pipeEngine.hpp"

namespace tsf {

inline std::unique_ptr<IEngine> get_engine() {
    return std::make_unique<PipeEngine>();
}
}  // namespace tsf
