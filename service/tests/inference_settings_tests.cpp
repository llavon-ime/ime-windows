#include "service/inference_settings_codec.hpp"

#include <ime-core/core.hpp>

#include <cstdlib>
#include <string>

namespace {

using llavon::ime::core::InferenceBackend;
using llavon::ime::core::InferenceDeviceSelection;
using llavon::service::inference_settings_codec::decode;
using llavon::service::inference_settings_codec::encode;

bool round_trips(InferenceDeviceSelection expected) {
    const auto decoded = decode(encode(expected));
    return decoded && decoded->backend == expected.backend &&
           decoded->device_id == expected.device_id;
}

}  // namespace

int main() {
    if (!round_trips({InferenceBackend::automatic, {}}) ||
        !round_trips({InferenceBackend::cpu, {}}) ||
        !round_trips({InferenceBackend::cuda, "0000:01:00.0"}) ||
        !round_trips({InferenceBackend::vulkan, "GPU-\xe6\xb8\xac\xe8\xa9\xa6"})) {
        return EXIT_FAILURE;
    }

    const auto defaulted = decode(R"({"schema":1,"inference":{"backend":"cpu"}})");
    if (!defaulted || defaulted->backend != InferenceBackend::cpu ||
        !defaulted->device_id.empty()) {
        return EXIT_FAILURE;
    }

    if (decode("not json") ||
        decode(R"({"schema":2,"inference":{"backend":"cpu","device_id":""}})") ||
        decode(R"({"schema":1,"inference":{"backend":"directml","device_id":"0"}})")) {
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
