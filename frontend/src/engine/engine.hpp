#pragma once

#include <memory>

namespace tsf {

class IEngine;

std::unique_ptr<IEngine> make_pipe_engine();

}  // namespace tsf
