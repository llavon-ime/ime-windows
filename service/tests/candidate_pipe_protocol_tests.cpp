#include "service/candidate_pipe_protocol.hpp"

namespace protocol = llavon::service::candidate_pipe_protocol;

int main() {
    if (!protocol::valid_presentation_header(9, 0, 1, 0, 0, 1)) {
        return 1;
    }
    if (!protocol::valid_presentation_header(36, 35, 4, 3, 1, 0)) {
        return 2;
    }
    if (protocol::valid_presentation_header(0, 0, 1, 0, 0, 0)) {
        return 3;
    }
    if (protocol::valid_presentation_header(10, 0, 1, 0, 0, 0)) {
        return 4;
    }
    if (protocol::valid_presentation_header(9, 9, 1, 0, 0, 0)) {
        return 5;
    }
    if (protocol::valid_presentation_header(9, 0, 0, 0, 0, 0)) {
        return 6;
    }
    if (protocol::valid_presentation_header(9, 0, 1, 1, 0, 0)) {
        return 7;
    }
    if (protocol::valid_presentation_header(9, 0, 1, 0, 2, 0)) {
        return 8;
    }
    return 0;
}
