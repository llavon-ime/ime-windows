#include <ime-core/core.hpp>
#include <windows.h>

#include "service/prediction_pipe_server.hpp"
#include "service/candidate_ui_loader.hpp"
#include "service/debug/core_logger_adapter.hpp"
#include "service/inference_settings.hpp"
#include "service/settings_ui_loader.hpp"
#include "service/tray_icon.hpp"

#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr const wchar_t* kModelFilename = L"llavon-ime-llama-250m-Q4_K_M.gguf";
constexpr const wchar_t* kModelPathEnv = L"LLAVON_IME_MODEL_PATH";
constexpr const wchar_t* kTablesDirEnv = L"LLAVON_IME_TABLES_DIR";

void print_usage(const char* executable) {
    std::cerr << "Usage: " << executable << " [<model-path> <tables-dir>]\n";
    std::cerr << "When no arguments are provided, paths are resolved from "
                 "LLAVON_IME_MODEL_PATH/LLAVON_IME_TABLES_DIR or the installed layout.\n";
}

std::optional<std::filesystem::path> environment_path(const wchar_t* name) {
    const DWORD required = GetEnvironmentVariableW(name, nullptr, 0);
    if (required == 0) {
        return std::nullopt;
    }

    std::wstring value(required, L'\0');
    const DWORD copied = GetEnvironmentVariableW(name, value.data(), required);
    if (copied == 0) {
        return std::nullopt;
    }

    value.resize(copied);
    return std::filesystem::path(value);
}

std::filesystem::path executable_directory() {
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD copied =
            GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (copied == 0) {
            throw std::runtime_error("failed to resolve executable path");
        }
        if (copied < buffer.size() - 1) {
            buffer.resize(copied);
            return std::filesystem::path(buffer).parent_path();
        }
        buffer.resize(buffer.size() * 2);
    }
}

std::filesystem::path default_model_path() {
    if (auto path = environment_path(kModelPathEnv)) {
        return *path;
    }
    return executable_directory().parent_path() / "models" / kModelFilename;
}

std::filesystem::path default_tables_directory() {
    if (auto path = environment_path(kTablesDirEnv)) {
        return *path;
    }
    return executable_directory().parent_path() / "tables";
}

void launch_debugger() noexcept {
    try {
        const auto debugger_path = executable_directory() / L"llavon-ime-debugger.exe";
        std::wstring command_line = L"\"" + debugger_path.wstring() + L"\"";
        STARTUPINFOW startup{sizeof(startup)};
        PROCESS_INFORMATION process{};
        if (CreateProcessW(debugger_path.c_str(), command_line.data(), nullptr, nullptr, FALSE, 0,
                           nullptr, debugger_path.parent_path().c_str(), &startup, &process)) {
            CloseHandle(process.hThread);
            CloseHandle(process.hProcess);
        } else {
            std::cerr << "[WARN] unable to launch debugger: " << GetLastError() << '\n';
        }
    } catch (...) {
        std::cerr << "[WARN] unable to launch debugger\n";
    }
}

llavon::ime::core::CoreConfig parse_core_config(int argc, char* argv[]) {
    llavon::ime::core::CoreConfig config;
    if (argc == 1) {
        config.model_path = default_model_path();
        config.tables_dir = default_tables_directory();
        return config;
    }
    if (argc == 3) {
        config.model_path = argv[1];
        config.tables_dir = argv[2];
        return config;
    }

    print_usage(argv[0]);
    throw std::invalid_argument("invalid command line");
}

int run_server(
    std::shared_ptr<llavon::ime::core::Core> core,
    llavon::service::CandidateUiLoader& candidate_ui) noexcept {
    try {
        llavon::service::PredictionPipeServer server(std::move(core), candidate_ui);
        std::clog << "[SRV] prediction transport: " << server.name() << '\n';
        return server.run();
    } catch (const std::exception& error) {
        std::cerr << "[ERR] fatal: " << error.what() << '\n';
        return 1;
    }
}

}  // namespace

int main(int argc, char* argv[]) {
    std::clog << "[SRV] IME Windows Service starting\n";
    try {
        if (argc == 2 && std::string(argv[1]) == "--help") {
            print_usage(argv[0]);
            return 0;
        }

        auto config = parse_core_config(argc, argv);
        config.logger = std::make_shared<llavon::service::debug::CoreLoggerAdapter>();
        const auto inference_settings = llavon::service::load_inference_settings();
        config.inference_device = inference_settings;

        std::vector<llavon::ime::core::InferenceDeviceInfo> inference_devices;
        try {
            inference_devices = llavon::ime::core::enumerate_inference_devices();
        } catch (const std::exception& error) {
            std::clog << "[WARN] unable to enumerate inference devices: " << error.what() << '\n';
        }
        auto core = std::make_shared<llavon::ime::core::Core>(std::move(config));
        const auto active_inference = core->inference_runtime_info();

        llavon::service::CandidateUiLoader candidate_ui;
        llavon::service::SettingsUiLoader settings_ui;
        settings_ui.configure(
            inference_devices, inference_settings, active_inference,
            [](const llavon::ime::core::InferenceDeviceSelection& selection) {
                return llavon::service::save_inference_settings(selection);
            });
        llavon::service::TrayIcon tray;
        if (!tray.create(GetModuleHandleW(nullptr), [&settings_ui] { settings_ui.show(); },
                         [] { launch_debugger(); })) {
            std::cerr << "[WARN] tray initialization failed: " << GetLastError() << '\n';
            return run_server(std::move(core), candidate_ui);
        }

        int server_result = 1;
        std::thread server_thread([&tray, &candidate_ui, &server_result, core = std::move(core)]() mutable {
            server_result = run_server(std::move(core), candidate_ui);
            tray.notify_server_stopped(server_result);
        });

        tray.run_message_loop();
        server_thread.join();
        return server_result;
    } catch (const std::invalid_argument& error) {
        if (std::string(error.what()) != "invalid command line") {
            std::cerr << "[ERR] fatal: " << error.what() << '\n';
        }
        return 1;
    } catch (const std::exception& error) {
        std::cerr << "[ERR] fatal: " << error.what() << '\n';
        return 1;
    }
}
