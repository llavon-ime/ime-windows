#include <ime-core/core.hpp>
#include <utf8/cpp20.h>
#include <windows.h>
#include <shlobj.h>

#include "service/prediction_pipe_server.hpp"
#include "service/candidate_ui_loader.hpp"
#include "service/custom_name_matcher.hpp"
#include "service/debug/core_logger_adapter.hpp"
#include "service/lora_training_manager.hpp"
#include "service/user_settings.hpp"
#include "service/settings_ui_loader.hpp"
#include "service/tray_icon.hpp"

#include <algorithm>
#include <exception>
#include <filesystem>
#include <iostream>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr const wchar_t* kModelFilename = L"llavon-ime-llama-250m-Q4_K_M.gguf";
constexpr const wchar_t* kModelPathEnv = L"LLAVON_IME_MODEL_PATH";
constexpr const wchar_t* kTablesDirEnv = L"LLAVON_IME_TABLES_DIR";
constexpr const wchar_t* kServiceInstanceMutexName = L"Local\\LlavonImeServiceInstance";

class ServiceInstanceLock {
public:
    ServiceInstanceLock() {
        handle_ = CreateMutexW(nullptr, TRUE, kServiceInstanceMutexName);
        if (!handle_) {
            throw std::runtime_error(
                "failed to create service instance mutex: " + std::to_string(GetLastError()));
        }

        if (GetLastError() == ERROR_ALREADY_EXISTS) {
            CloseHandle(handle_);
            handle_ = nullptr;
        }
    }

    ~ServiceInstanceLock() {
        if (!handle_) return;
        ReleaseMutex(handle_);
        CloseHandle(handle_);
    }

    ServiceInstanceLock(const ServiceInstanceLock&) = delete;
    ServiceInstanceLock& operator=(const ServiceInstanceLock&) = delete;

    bool owns_instance() const noexcept { return handle_ != nullptr; }

private:
    HANDLE handle_ = nullptr;
};

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
    PWSTR raw = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramData, KF_FLAG_DEFAULT,
                                       nullptr, &raw))) {
        const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> owned(raw, CoTaskMemFree);
        const auto root = std::filesystem::path(owned.get()) / L"Llavon IME" / L"models";
        std::ifstream marker(root / L"current.revision", std::ios::binary);
        std::string revision;
        if (marker >> revision && revision.size() == 40 &&
            std::ranges::all_of(revision, [](char c) {
                return c >= '0' && c <= '9' || c >= 'a' && c <= 'f' ||
                       c >= 'A' && c <= 'F';
            })) {
            const auto candidate = root /
                std::wstring(revision.begin(), revision.end()) / kModelFilename;
            std::error_code ignored;
            if (std::filesystem::is_regular_file(candidate, ignored))
                return candidate;
        }
    }
    return executable_directory().parent_path() / "models" / kModelFilename;
}

std::filesystem::path resolve_configured_model_path(std::filesystem::path path) {
    if (path.is_relative()) {
        path = executable_directory().parent_path() / path;
    }
    return path.lexically_normal();
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
    llavon::service::PredictionPipeServer& server) noexcept {
    try {
        std::clog << "[SRV] prediction transport: " << server.name() << '\n';
        return server.run();
    } catch (const std::exception& error) {
        std::cerr << "[ERR] fatal: " << error.what() << '\n';
        return 1;
    }
}

}  // namespace

int main(int argc, char* argv[]) {
    try {
        if (argc == 2 && std::string(argv[1]) == "--help") {
            print_usage(argv[0]);
            return 0;
        }

        ServiceInstanceLock instance_lock;
        if (!instance_lock.owns_instance()) {
            std::clog << "[SRV] IME Windows Service already running; exiting\n";
            return 0;
        }

        std::clog << "[SRV] IME Windows Service starting\n";

        auto config = parse_core_config(argc, argv);
        config.logger = std::make_shared<llavon::service::debug::CoreLoggerAdapter>();
        const auto user_settings = llavon::service::load_settings();
        if (argc == 1 && !user_settings.model_path.empty()) {
            config.model_path = resolve_configured_model_path(
                std::filesystem::path(utf8::utf8to16(user_settings.model_path)));
        }
        config.inference_device = user_settings.inference;

        std::vector<llavon::ime::core::InferenceDeviceInfo> inference_devices;
        try {
            inference_devices = llavon::ime::core::enumerate_inference_devices();
        } catch (const std::exception& error) {
            std::clog << "[WARN] unable to enumerate inference devices: " << error.what() << '\n';
        }
        auto core = std::make_shared<llavon::ime::core::Core>(config);
        const auto active_inference = core->inference_runtime_info();

        llavon::service::CandidateUiLoader candidate_ui;
        auto custom_names =
            std::make_shared<llavon::service::CustomNameMatcher>(user_settings.custom_names);
        llavon::service::PredictionPipeServer server(
            std::move(core), candidate_ui, custom_names);
        llavon::service::LoraTrainingManager lora_training(
            server.training_data_writer(), config.tables_dir);
        auto custom_names_update_mutex = std::make_shared<std::mutex>();
        llavon::service::SettingsUiLoader settings_ui;
        auto active_config = std::make_shared<llavon::ime::core::CoreConfig>(config);
        std::u16string displayed_model_path;
        if (argc != 1) {
            displayed_model_path = config.model_path.u16string();
        } else if (!user_settings.model_path.empty()) {
            displayed_model_path = utf8::utf8to16(user_settings.model_path);
        } else if (environment_path(kModelPathEnv)) {
            displayed_model_path = config.model_path.u16string();
        } else {
            displayed_model_path = config.model_path.u16string();
        }
        settings_ui.configure(
            inference_devices, user_settings.inference, active_inference,
            [&server, active_config](
                const llavon::ime::core::InferenceDeviceSelection& selection) {
                try {
                    auto reload_config = *active_config;
                    reload_config.inference_device = selection;
                    (void)server.replace_core(std::move(reload_config));
                    if (!llavon::service::save_inference_settings(selection)) return false;
                    active_config->inference_device = selection;
                    return true;
                } catch (const std::exception& error) {
                    std::cerr << "[ERR] unable to reload inference core: "
                              << error.what() << '\n';
                    return false;
                }
            },
            std::move(displayed_model_path),
            [&server, active_config, &lora_training](
                const std::filesystem::path& model_path) {
                try {
                    const auto resolved_model_path =
                        resolve_configured_model_path(model_path);
                    std::error_code error;
                    if (!std::filesystem::is_regular_file(resolved_model_path, error) &&
                        !lora_training.ensure_model_exported(resolved_model_path)) {
                        return false;
                    }

                    auto reload_config = *active_config;
                    reload_config.model_path = resolved_model_path;
                    (void)server.replace_core(std::move(reload_config));
                    const auto model_path_utf8 = utf8::utf16to8(model_path.u16string());
                    if (!llavon::service::save_model_path(model_path_utf8)) return false;
                    active_config->model_path = resolved_model_path;
                    lora_training.on_model_applied(resolved_model_path);
                    return true;
                } catch (const std::exception& error) {
                    std::cerr << "[ERR] unable to reload model: "
                              << error.what() << '\n';
                    return false;
                }
            },
            user_settings.custom_names,
            [custom_names, custom_names_update_mutex](
                const std::vector<llavon::service::CustomNameSetting>& settings) {
                // Keep persistent and in-memory revisions in the same order even if
                // more than one caller saves concurrently.
                std::lock_guard update_lock(*custom_names_update_mutex);
                if (!llavon::service::save_custom_names(settings)) return false;
                custom_names->replace(settings);
                return true;
            },
            user_settings.shift_space_width_toggle_enabled,
            [](bool enabled) {
                return llavon::service::save_shift_space_width_toggle_setting(enabled);
            },
            [&server](std::string_view password, std::int64_t base_run_id) {
                return server.training_data_writer()->pending_items(password, base_run_id);
            },
            [&server](std::u16string_view event_id) {
                return server.training_data_writer()->delete_pending(event_id);
            },
            [&server] {
                return server.training_data_writer()->lora_training_history();
            },
            [&lora_training](const std::vector<std::u16string>& selected_event_ids,
                             const llavon_settings_lora_options& source, std::string_view password) {
                llavon::service::LoraTrainingOptions options{
                    .base_run_id = source.base_run_id,
                    .rank = source.rank,
                    .alpha = source.alpha,
                    .dropout = source.dropout,
                    .batch_size = source.batch_size,
                    .gradient_accumulation = source.gradient_accumulation,
                    .epochs = source.epochs,
                    .max_steps = source.max_steps,
                    .learning_rate = source.learning_rate,
                    .weight_decay = source.weight_decay,
                    .warmup_steps = source.warmup_steps,
                    .max_gradient_norm = source.max_gradient_norm,
                    .save_every = source.save_every,
                    .device = source.device,
                    .seed = source.seed,
                    .shuffle = source.shuffle != 0,
                    .max_sequence_length = source.max_sequence_length,
                    .dtype = source.dtype ? source.dtype : u"float32",
                    .target_modules = source.target_modules
                        ? source.target_modules
                        : u"q_proj,v_proj",
                    .strength = static_cast<llavon::service::LoraTrainingStrength>(source.strength),
                    .only_manually_selected = source.only_manually_selected != 0,
                };
                return lora_training.start_training_async(
                    selected_event_ids, std::move(options), password);
            },
            [&lora_training] { return lora_training.status(); },
            [&lora_training](std::int32_t action) {
                switch (action) {
                case LLAVON_LORA_CHECK_MODEL: return lora_training.check_model_async();
                case LLAVON_LORA_DOWNLOAD_MODEL: return lora_training.download_model_async();
                case LLAVON_LORA_CHECK_TRAINER: return lora_training.check_trainer_async();
                case LLAVON_LORA_INSTALL_CPU: return lora_training.install_trainer_async(0);
                case LLAVON_LORA_INSTALL_CUDA: return lora_training.install_trainer_async(1);
                case LLAVON_LORA_INSTALL_ROCM: return lora_training.install_trainer_async(2);
                default: return false;
                }
            },
            [&lora_training] { lora_training.cancel(); },
            [&server, &lora_training](int action, std::string_view password) -> std::size_t {
                auto writer = server.training_data_writer();
                switch (action) {
                case LLAVON_PROTECTION_STATUS: {
                    const auto status = writer->protection_status();
                    return (status.configured ? 1u : 0u) | (status.enabled ? 2u : 0u);
                }
                case LLAVON_PROTECTION_SETUP: writer->configure_password(password); return 0;
                case LLAVON_PROTECTION_ENABLE: writer->set_recording_enabled(true); return 0;
                case LLAVON_PROTECTION_DISABLE: writer->set_recording_enabled(false); return 0;
                case LLAVON_PROTECTION_CLEANUP: return lora_training.discard_plaintext_datasets();
                case LLAVON_PROTECTION_RESET: lora_training.reset_conversation_data(); return 0;
                default: throw std::invalid_argument("unknown protection action");
                }
            });
        struct PendingCountRegistration {
            std::shared_ptr<llavon::service::TrainingDataWriter> writer;
            ~PendingCountRegistration() {
                writer->set_pending_count_callback({});
            }
        } pending_count_registration{server.training_data_writer()};
        pending_count_registration.writer->set_pending_count_callback(
            [&settings_ui](std::size_t count) {
                settings_ui.notify_pending_count(count);
            });
        llavon::service::TrayIcon tray;
        if (!tray.create(GetModuleHandleW(nullptr), [&settings_ui] { settings_ui.show(); },
                         [] { launch_debugger(); },
                         [&settings_ui](POINT location) {
                             settings_ui.show_context_menu(location);
                         },
                         [&server] { server.stop(); })) {
            std::cerr << "[WARN] tray initialization failed: " << GetLastError() << '\n';
            return run_server(server);
        }

        int server_result = 1;
        std::thread server_thread([&tray, &server, &server_result] {
            server_result = run_server(server);
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
