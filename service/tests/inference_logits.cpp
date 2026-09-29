#include <ime-core/ryzen_ai.hpp>
#include <llama.h>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
constexpr std::int32_t vocabulary_size = 18546;
class Log final : public llavon::ime::core::Logger {
public:
    void log(std::string message) noexcept override { std::cerr << message << '\n'; }
    void log(MessageFactory) noexcept override {}
};

std::string utf8_path(const std::filesystem::path& path) {
    const auto value = path.u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}
}

// Manual quality probe. Each input line is a prefix of token IDs; output is one
// little-endian float32 vocabulary row per line, for comparison with HF FP32.
int wmain(int argc, wchar_t** argv) {
    static_assert(std::endian::native == std::endian::little && sizeof(float) == 4);
    try {
        if (argc != 5 && argc != 6) throw std::invalid_argument("Expected cpu|npu model token-file logits-file [NPU cache]");
        const std::wstring_view backend(argv[1]);
        const bool npu = backend == L"npu";
        if ((npu && argc != 6) || (!npu && (backend != L"cpu" || argc != 5))) {
            throw std::invalid_argument("Invalid backend/arguments");
        }
        std::ifstream input{std::filesystem::path(argv[3])};
        input.exceptions(std::ios::badbit);
        if (!input) throw std::runtime_error("Unable to open token file");
        std::vector<std::vector<std::int32_t>> prefixes;
        for (std::string line; std::getline(input, line);) {
            std::istringstream stream(line);
            std::vector<std::int32_t> tokens;
            for (std::int32_t token; stream >> token;) {
                if (token < 0 || token >= vocabulary_size) throw std::runtime_error("Invalid token ID");
                tokens.push_back(token);
            }
            if (stream.eof() && tokens.empty()) continue; // Separators reset native-probe KV; CPU rows are always fresh.
            if (!stream.eof() || tokens.size() >= 384) throw std::runtime_error("Invalid token prefix");
            prefixes.push_back(std::move(tokens));
        }
        if (prefixes.empty()) throw std::runtime_error("No token prefixes");

        std::shared_ptr<llavon::ime::core::InferenceAccelerator> accelerator;
        std::unique_ptr<llama_model, decltype(&llama_model_free)> model(nullptr, llama_model_free);
        std::unique_ptr<llama_context, decltype(&llama_free)> context(nullptr, llama_free);
        if (npu) {
            accelerator = llavon::ime::core::create_ryzen_ai_accelerator({
                .model_directory = std::filesystem::absolute(argv[2]),
                .cache_directory = std::filesystem::absolute(argv[5]),
                .allow_compilation = false, .logger = std::make_shared<Log>()});
            accelerator->prepare({}, 384);
        } else {
            llama_backend_init();
            auto model_options = llama_model_default_params();
            std::array<ggml_backend_dev_t, 1> devices{};
            model_options.devices = devices.data();
            model_options.n_gpu_layers = 0;
            model.reset(llama_model_load_from_file(utf8_path(argv[2]).c_str(), model_options));
            if (!model || llama_vocab_n_tokens(llama_model_get_vocab(model.get())) != vocabulary_size) {
                throw std::runtime_error("Unable to load expected GGUF model");
            }
            auto options = llama_context_default_params();
            options.n_ctx = 384;
            options.n_batch = 384;
            options.n_ubatch = 384;
            options.n_threads = 8;
            options.n_threads_batch = 8;
            options.offload_kqv = false;
            options.op_offload = false;
            context.reset(llama_init_from_model(model.get(), options));
            if (!context) throw std::runtime_error("Unable to create CPU context");
        }
        std::ofstream output{std::filesystem::path(argv[4]), std::ios::binary};
        output.exceptions(std::ios::badbit | std::ios::failbit);
        auto next_request = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
        for (auto& tokens : prefixes) {
            std::this_thread::sleep_until(next_request);
            next_request = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
            std::vector<float> values;
            if (npu) {
                auto decoder = accelerator->create_context();
                decoder->decode(tokens, 0);
                const auto logits = decoder->logits();
                values.assign(logits.begin(), logits.end());
            } else {
                llama_memory_clear(llama_get_memory(context.get()), true);
                auto batch = llama_batch_get_one(tokens.data(), static_cast<std::int32_t>(tokens.size()));
                if (llama_decode(context.get(), batch) != 0) throw std::runtime_error("CPU decode failed");
                const auto* logits = llama_get_logits_ith(context.get(), -1);
                if (!logits) throw std::runtime_error("Missing CPU logits");
                values.assign(logits, logits + vocabulary_size);
            }
            if (values.size() != vocabulary_size || !std::ranges::all_of(values, [](float v) { return std::isfinite(v); })) {
                throw std::runtime_error("Invalid logits");
            }
            output.write(reinterpret_cast<const char*>(values.data()), static_cast<std::streamsize>(values.size() * sizeof(float)));
        }
        output.close();
        context.reset();
        model.reset();
        if (!npu) llama_backend_free();
        std::cout << "Wrote " << prefixes.size() << " vocabulary rows\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
