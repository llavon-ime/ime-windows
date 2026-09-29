#include <ime-core/ryzen_ai.hpp>
#include <ort_genai.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <format>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
class TestLogger final : public llavon::ime::core::Logger {
public:
    void log(std::string message) noexcept override { std::cerr << message << '\n'; }
    void log(MessageFactory) noexcept override {}
};

std::string utf8_path(const std::filesystem::path& path) {
    const auto value = path.u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}
} // namespace

// Opt-in hardware test: npu-hardware-tests <NPU model> <cache> <CPU reference>.
// The reference must use the same asymmetric INT4/128 weights before AMD conversion.
int wmain(int argc, wchar_t** argv) {
    try {
        if (argc != 4) throw std::invalid_argument("Expected NPU model, compiled cache and CPU reference directories");
        auto accelerator = llavon::ime::core::create_ryzen_ai_accelerator({
            .model_directory = std::filesystem::absolute(argv[1]),
            .cache_directory = std::filesystem::absolute(argv[2]),
            .allow_compilation = false, .logger = std::make_shared<TestLogger>()});
        accelerator->prepare({}, 384);
        auto config = OgaConfig::Create(utf8_path(std::filesystem::absolute(argv[3])).c_str());
        config->ClearProviders();
        auto reference = OgaModel::Create(*config);
        std::size_t samples = 0;
        std::size_t matching = 0;
        double minimum_cosine = 1.0;
        double maximum_error = 0.0;
        double decode_ms = 0.0;
        for (const std::size_t length : {3u, 17u, 255u, 256u, 383u}) {
            auto context = accelerator->create_context();
            auto params = OgaGeneratorParams::Create(*reference);
            params->SetSearchOption("max_length", 384);
            auto cpu = OgaGenerator::Create(*reference, *params);
            std::vector<std::int32_t> tokens(length);
            tokens[0] = 1;
            for (std::size_t i = 1; i < length; ++i) tokens[i] = static_cast<std::int32_t>(100 + (i * 37) % 1000);
            const auto compare = [&](std::span<const std::int32_t> input, std::uint32_t position) {
                const auto start = std::chrono::steady_clock::now();
                context->decode(input, position);
                decode_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
                cpu->AppendTokens(input);
                const auto tensor = cpu->GetLogits();
                const auto actual = context->logits();
                const auto dims = tensor->Shape();
                std::size_t elements = 1;
                for (const auto dimension : dims) elements *= static_cast<std::size_t>(dimension);
                if (actual.size() != 18546 || tensor->Type() != OgaElementType_float32 || elements < actual.size()) {
                    throw std::runtime_error("Invalid reference/logits dimensions");
                }
                const auto* data = static_cast<const float*>(tensor->Data()) + elements - actual.size();
                const std::span expected(data, actual.size());
                double dot = 0.0, norm_a = 0.0, norm_b = 0.0;
                for (std::size_t i = 0; i < actual.size(); ++i) {
                    if (!std::isfinite(actual[i]) || !std::isfinite(expected[i])) {
                        throw std::runtime_error("Non-finite comparison logits");
                    }
                    dot += static_cast<double>(actual[i]) * expected[i];
                    norm_a += static_cast<double>(actual[i]) * actual[i];
                    norm_b += static_cast<double>(expected[i]) * expected[i];
                    maximum_error = (std::max)(maximum_error, std::abs(static_cast<double>(actual[i]) - expected[i]));
                }
                const double cosine = dot / std::sqrt(norm_a * norm_b);
                minimum_cosine = (std::min)(minimum_cosine, cosine);
                if (!std::isfinite(cosine)) throw std::runtime_error("Invalid logits cosine similarity");
                std::cout << std::format("length={} position={} cosine={:.6f} max_error={:.6f} NPU top={} CPU top={}\n",
                    input.size(), position, cosine, maximum_error,
                    std::ranges::max_element(actual) - actual.begin(),
                    std::ranges::max_element(expected) - expected.begin());
                matching += static_cast<std::size_t>(
                    std::ranges::max_element(actual) - actual.begin() ==
                    std::ranges::max_element(expected) - expected.begin());
                ++samples;
            };
            compare(tokens, 0);
            const std::int32_t next = 123;
            compare(std::span(&next, 1), static_cast<std::uint32_t>(length));
            context->truncate(1);
            cpu->RewindTo(1);
            compare(std::span(tokens).subspan(1), 1);
            context->truncate(static_cast<std::uint32_t>(length - 2));
            cpu->RewindTo(length - 2);
            compare(std::array<std::int32_t, 2>{123, 444}, static_cast<std::uint32_t>(length - 2));
            context->truncate(static_cast<std::uint32_t>(length - 1));
            cpu->RewindTo(length - 1);
            compare(std::array<std::int32_t, 1>{777}, static_cast<std::uint32_t>(length - 1));
            context->truncate(0);
            cpu->RewindTo(0);
            compare(std::span(tokens).first(1), 0);
            compare(std::span(tokens).subspan(1), 1);
        }
        std::cout << std::format("{} NPU/CPU comparisons; top-1 {}/{}; min cosine {:.6f}; max error {:.6f}; NPU total {:.1f} ms\n",
            samples, matching, samples, minimum_cosine, maximum_error, decode_ms);
        if (minimum_cosine < 0.99) throw std::runtime_error("NPU logits diverge from CPU reference");
        if (matching * 10 < samples * 9) throw std::runtime_error("NPU/CPU top-1 agreement below 90%");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
