#include "engine/engine.hpp"

#include "engine/impl/pipeEngine.hpp"

namespace tsf {

std::unique_ptr<IEngine> make_pipe_engine() {
    return std::make_unique<PipeEngine>();
}

}  // namespace tsf
