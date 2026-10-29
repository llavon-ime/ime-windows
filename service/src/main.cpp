#include <ime-core/core.hpp>
#include <utf8/cpp20.h>
#include <windows.h>
#include <shlobj.h>
#include <winrt/base.h>

#include "service/prediction_pipe_server.hpp"
#include "service/amd_gpu_latency_boost.hpp"
#include "candidate/candidate_ui.hpp"
#include "service/custom_name_matcher.hpp"
#include "service/debug/core_logger_adapter.hpp"
#include "service/lora_training_manager.hpp"
#include "service/major_update_notifier.hpp"
#include "service/process_mta.hpp"
#include "service/user_settings.hpp"
#include "settings/settings_ui.hpp"
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

// Password conversion is scoped to the operation and wiped before returning.
struct TransientPassword {
    std::string value;
    explicit TransientPassword(std::u16string_view password) : value(utf8::utf16to8(password)) {}
    ~TransientPassword() {
        if (!value.empty()) SecureZeroMemory(value.data(), value.size());
    }
};

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

std::filesystem::path installed_base_model_path() {
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

std::filesystem::path default_model_path() {
    if (auto path = environment_path(kModelPathEnv)) {
        return *path;
    }
    return installed_base_model_path();
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

std::filesystem::path vulkan_pipeline_cache_directory() noexcept {
    try {
        PWSTR raw = nullptr;
        if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE,
                                        nullptr, &raw))) {
            return {};
        }
        const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> owned(raw, CoTaskMemFree);
        return std::filesystem::path(owned.get()) / L"Llavon IME" / L"vulkan-cache";
    } catch (...) {
        return {};
    }
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
    if (const auto worker = llavon::service::run_amd_gpu_latency_boost_worker(argc, argv)) {
        return *worker;
    }
    try {
        const llavon::service::ProcessMta process_mta;
        if (argc == 2 && std::string(argv[1]) == "--help") {
            print_usage(argv[0]);
            return 0;
        }

        const bool show_settings = argc == 2 && std::string(argv[1]) == "--settings";
        const int service_argc = show_settings ? 1 : argc;

        ServiceInstanceLock instance_lock;
        if (!instance_lock.owns_instance()) {
            if (show_settings) {
                const HWND existing = FindWindowW(L"LlavonImeServiceTrayWindow", nullptr);
                const UINT message = RegisterWindowMessageW(L"LlavonIme.OpenSettings");
                if (existing && message) PostMessageW(existing, message, 0, 0);
            }
            std::clog << "[SRV] IME Windows Service already running; exiting\n";
            return 0;
        }

        std::clog << "[SRV] IME Windows Service starting\n";

        llavon::ui::UiThread ui_thread;

        auto config = parse_core_config(service_argc, argv);
        config.logger = std::make_shared<llavon::service::debug::CoreLoggerAdapter>();
        config.sessions_per_context =
            static_cast<std::uint32_t>(llavon::service::prediction_pipe::SessionLru::capacity);
        config.vulkan_pipeline_cache_dir = vulkan_pipeline_cache_directory();
        if (config.vulkan_pipeline_cache_dir.empty()) {
            std::clog << "[WARN] Vulkan pipeline cache disabled: no directory supplied\n";
        }
        const auto user_settings = llavon::service::load_settings();
        if (service_argc == 1 && !user_settings.model_path.empty()) {
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

        llavon::candidate::CandidateUi candidate_ui(ui_thread);
        auto custom_names =
            std::make_shared<llavon::service::CustomNameMatcher>(user_settings.custom_names);
        llavon::service::PredictionPipeServer server(
            std::move(core), candidate_ui, custom_names,
            user_settings.gpu_boost_enabled);
        llavon::service::LoraTrainingManager lora_training(
            server.training_data_writer(), config.tables_dir);
        auto custom_names_update_mutex = std::make_shared<std::mutex>();
        llavon::settings::SettingsConfiguration settings;
        auto active_config = std::make_shared<llavon::ime::core::CoreConfig>(config);
        std::u16string displayed_model_path;
        if (service_argc != 1) {
            displayed_model_path = config.model_path.u16string();
        } else if (!user_settings.model_path.empty()) {
            displayed_model_path = utf8::utf8to16(user_settings.model_path);
        } else if (environment_path(kModelPathEnv)) {
            displayed_model_path = config.model_path.u16string();
        } else {
            displayed_model_path = config.model_path.u16string();
        }
        const auto device_option = [](const llavon::ime::core::InferenceDeviceInfo& device) {
            return llavon::settings::InferenceDeviceOption{
                .backend = device.backend,
                .device_type = device.type,
                .device_id = utf8::utf8to16(device.device_id),
                .name = utf8::utf8to16(device.name),
                .description = utf8::utf8to16(device.description),
                .memory_total = device.memory_total,
            };
        };
        for (const auto& device : inference_devices)
            settings.devices.push_back(device_option(device));
        settings.selected_backend = user_settings.inference.backend;
        settings.selected_device_id = utf8::utf8to16(user_settings.inference.device_id);
        settings.active_device = device_option(active_inference.device);
        settings.gpu_offload = active_inference.gpu_offload;
        settings.fell_back_to_cpu = active_inference.fell_back_to_cpu;
        settings.model_path = std::move(displayed_model_path);
        settings.base_model_path = installed_base_model_path().u16string();
        for (const auto& name : user_settings.custom_names) {
            settings.custom_names.push_back(
                {utf8::utf8to16(utf8::utf32to8(name.name)), name.readings});
        }
        settings.shift_space_width_toggle_enabled = user_settings.shift_space_width_toggle_enabled;
        settings.immediate_conversion_enabled = user_settings.immediate_conversion_enabled;
        settings.gpu_boost_enabled = user_settings.gpu_boost_enabled;
        settings.major_update_notifications_enabled =
            user_settings.major_update_notifications_enabled;
        settings.actions.save_inference = [&server, active_config](
                                              llavon::settings::InferenceBackend backend,
                                              std::u16string_view device_id) {
            try {
                llavon::ime::core::InferenceDeviceSelection selection{
                    backend, utf8::utf16to8(device_id)};
                if (backend == llavon::settings::InferenceBackend::automatic ||
                    backend == llavon::settings::InferenceBackend::cpu)
                    selection.device_id.clear();
                auto reload_config = *active_config;
                reload_config.inference_device = selection;
                (void)server.replace_core(std::move(reload_config));
                if (!llavon::service::save_inference_settings(selection)) return false;
                active_config->inference_device = selection;
                return true;
            } catch (const std::exception& error) {
                std::cerr << "[ERR] unable to reload inference core: " << error.what() << '\n';
                return false;
            } catch (...) {
                std::cerr << "[ERR] unable to reload inference core: unknown error\n";
                return false;
            }
        };
        settings.actions.save_model_path =
            [&server, active_config, &lora_training](const std::filesystem::path& model_path) {
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
                } catch (...) {
                    std::cerr << "[ERR] unable to reload model: unknown error\n";
                    return false;
                }
            };
        settings.actions.prepare_model = [active_config, &lora_training](
                                             const std::filesystem::path& model_path,
                                             llavon::settings::ModelPreparation action) {
            try {
                const auto resolved_model_path = resolve_configured_model_path(model_path);
                if (action == llavon::settings::ModelPreparation::prepare)
                    return lora_training.ensure_model_exported(resolved_model_path);
                if (action == llavon::settings::ModelPreparation::discard &&
                    resolved_model_path != active_config->model_path)
                    return lora_training.discard_model_export(resolved_model_path);
                return false;
            } catch (...) {
                return false;
            }
        };
        settings.actions.save_custom_names =
            [custom_names, custom_names_update_mutex](
                const std::vector<llavon::settings::CustomNameOption>& names) {
                try {
                    std::vector<llavon::service::CustomNameSetting> settings;
                    for (const auto& name : names)
                        settings.push_back(
                            {utf8::utf8to32(utf8::utf16to8(name.name)), name.readings});
                    // Keep persistent and in-memory revisions in the same order even if
                    // more than one caller saves concurrently.
                    std::lock_guard update_lock(*custom_names_update_mutex);
                    if (!llavon::service::save_custom_names(settings)) return false;
                    custom_names->replace(settings);
                    return true;
                } catch (...) {
                    return false;
                }
            };
        settings.actions.save_width_toggle = [](bool enabled) {
            return llavon::service::save_shift_space_width_toggle_setting(enabled);
        };
        settings.actions.save_immediate_conversion = [](bool enabled) {
            return llavon::service::save_immediate_conversion_setting(enabled);
        };
        settings.actions.save_gpu_boost = [&server](bool enabled) {
            try {
                if (!llavon::service::save_gpu_boost_setting(enabled)) return false;
                server.set_gpu_boost_enabled(enabled);
                return true;
            } catch (...) {
                return false;
            }
        };
        settings.actions.save_update_notifications = [](bool enabled) {
            return llavon::service::save_major_update_notifications_setting(enabled);
        };
        settings.actions.load_training_items = [&server](std::u16string_view password,
                                                   std::int64_t base_run_id)
            -> std::optional<std::vector<llavon::settings::TrainingDataOption>> {
            try {
                TransientPassword secret(password);
                return server.training_data_writer()->pending_items(secret.value, base_run_id);
            } catch (...) {
                return std::nullopt;
            }
        };
        settings.actions.delete_training_item = [&server](std::u16string_view event_id) {
            return server.training_data_writer()->delete_pending(event_id);
        };
        settings.actions.load_lora_history = [&server] {
            return server.training_data_writer()->lora_training_history();
        };
        settings.actions.load_lora_base_models = [&server] {
            return server.training_data_writer()->lora_base_models();
        };
        settings.actions.start_lora_training = [&lora_training](
                                                   const std::vector<std::u16string>& event_ids,
                                                   llavon::service::LoraTrainingOptions options,
                                                   std::u16string_view password) {
            try {
                TransientPassword secret(password);
                return lora_training.start_training_async(
                    event_ids, std::move(options), secret.value);
            } catch (...) {
                return false;
            }
        };
        settings.actions.get_lora_status =
            [&lora_training]() -> std::optional<llavon::service::LoraOperationStatus> {
            try {
                return lora_training.status();
            } catch (...) {
                return std::nullopt;
            }
        };
        settings.actions.lora_model_action = [&lora_training](llavon::settings::LoraAction action) {
            try {
                switch (action) {
                case llavon::settings::LoraAction::check_updates:
                    return lora_training.check_updates_async();
                case llavon::settings::LoraAction::download_model:
                    return lora_training.download_model_async();
                case llavon::settings::LoraAction::install_trainer:
                    return lora_training.install_trainer_async();
                case llavon::settings::LoraAction::install_cuda_runtime:
                    return lora_training.install_cuda_runtime_async();
                case llavon::settings::LoraAction::remove_cuda_runtime:
                    return lora_training.remove_cuda_runtime_async();
                default:
                    return false;
                }
            } catch (...) {
                return false;
            }
        };
        settings.actions.cancel_lora = [&lora_training] { lora_training.cancel(); };
        const auto writer = server.training_data_writer();
        settings.actions.protection.status =
            [writer]() -> std::optional<llavon::settings::ProtectionStatus> {
            try {
                const auto status = writer->protection_status();
                return llavon::settings::ProtectionStatus{status.configured, status.enabled};
            } catch (...) {
                return std::nullopt;
            }
        };
        settings.actions.protection.setup = [writer](std::u16string_view password) {
            try {
                TransientPassword secret(password);
                writer->configure_password(secret.value);
                return true;
            } catch (...) {
                return false;
            }
        };
        settings.actions.protection.set_enabled = [writer](bool enabled) {
            try {
                writer->set_recording_enabled(enabled);
                return true;
            } catch (...) {
                return false;
            }
        };
        settings.actions.protection.cleanup = [&lora_training]() -> std::optional<std::size_t> {
            try {
                return lora_training.discard_plaintext_datasets();
            } catch (...) {
                return std::nullopt;
            }
        };
        settings.actions.protection.reset = [&lora_training] {
            try {
                lora_training.reset_conversation_data();
                return true;
            } catch (...) {
                return false;
            }
        };
        settings.training_items = server.training_data_writer()->pending_items();
        llavon::settings::SettingsUi settings_ui(ui_thread, std::move(settings));

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
        if (show_settings) settings_ui.show();
        llavon::service::MajorUpdateNotifier update_notifier(
            [&tray] { tray.request_open_settings(); });

        int server_result = 1;
        std::thread server_thread([&tray, &server, &server_result] {
            server_result = run_server(server);
            tray.notify_server_stopped(server_result);
        });

        tray.run_message_loop();
        server_thread.join();
        return server_result;
    } catch (const winrt::hresult_error& error) {
        std::cerr << "[ERR] WinUI startup failed: " << winrt::to_string(error.message()) << '\n';
        return 1;
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
