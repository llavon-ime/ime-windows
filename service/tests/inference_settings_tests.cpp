#include "service/user_settings_codec.hpp"

#include <iostream>

int main() {
    using llavon::ime::core::InferenceBackend;
    using namespace llavon::service;
    const auto npu = user_settings_codec::decode(
        R"({"schema":1,"inference":{"backend":"ryzen_ai","device_id":"RYZENAI-NPU"},"model_path":"model.gguf"})");
    if (!npu || npu->inference.backend != InferenceBackend::ryzen_ai ||
        npu->inference.device_id != "RYZENAI-NPU") return 1;
    const auto restored = user_settings_codec::decode(user_settings_codec::encode(*npu));
    if (!restored || restored->inference.backend != InferenceBackend::ryzen_ai ||
        restored->inference.device_id != "RYZENAI-NPU" || restored->model_path != "model.gguf") return 2;
    auto cpu = *restored;
    cpu.inference.backend = InferenceBackend::cpu;
    const auto switched = user_settings_codec::decode(user_settings_codec::encode(cpu));
    if (!switched || switched->inference.backend != InferenceBackend::cpu ||
        !switched->inference.device_id.empty()) return 3;
    if (user_settings_codec::decode(R"({"schema":1,"inference":{"backend":"bogus"}})")) return 4;
    std::cout << "NPU settings round trip and CPU selection passed\n";
}
