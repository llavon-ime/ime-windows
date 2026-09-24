#include "lora_training_manager.hpp"
#include "training_data_cleanup.hpp"

#include "lora_dataset_builder.hpp"
#include "winrt_http.hpp"

#include <rfl/json.hpp>
#include <shlobj.h>
#include <utf8/cpp20.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <fstream>
#include <format>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace llavon::service {
namespace {

constexpr wchar_t assets_path_environment[] = L"LLAVON_IME_LORA_ASSETS_DIR";
constexpr wchar_t trainer_path_environment[] = L"LLAVON_IME_LORA_CLI_PATH";
constexpr char model_repository[] = "tony65535/llavon-ime-llama-250m";
constexpr wchar_t model_repository_directory[] =
    L"tony65535--llavon-ime-llama-250m";
constexpr std::array<std::string_view, 3> model_files{
    "config.json", "ime_vocab.json", "model.safetensors"};

std::optional<std::filesystem::path> environment_path(const wchar_t* name) {
    const DWORD required = GetEnvironmentVariableW(name, nullptr, 0);
    if (required == 0) return std::nullopt;
    std::wstring value(required, L'\0');
    const DWORD copied = GetEnvironmentVariableW(name, value.data(), required);
    if (copied == 0 || copied >= required) return std::nullopt;
    value.resize(copied);
    return std::filesystem::path(std::move(value));
}

std::filesystem::path local_app_data_root() {
    PWSTR value = nullptr;
    const HRESULT result =
        SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &value);
    if (FAILED(result)) throw std::runtime_error("unable to resolve LocalAppData");
    const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> owned(value, CoTaskMemFree);
    return std::filesystem::path(owned.get()) / L"Llavon IME";
}

std::filesystem::path executable_directory() {
    std::wstring value(MAX_PATH, L'\0');
    for (;;) {
        const DWORD copied = GetModuleFileNameW(nullptr, value.data(),
                                                static_cast<DWORD>(value.size()));
        if (copied == 0) throw std::runtime_error("unable to locate service executable");
        if (copied < value.size() - 1) {
            value.resize(copied);
            return std::filesystem::path(value).parent_path();
        }
        value.resize(value.size() * 2);
    }
}

std::filesystem::path trainer_executable() {
    if (auto configured = environment_path(trainer_path_environment)) return *configured;
    const auto adjacent = executable_directory() / L"tools" / L"lora" /
                          L"llavon-lora.exe";
    std::error_code error;
    if (std::filesystem::is_regular_file(adjacent, error)) return adjacent;

    PWSTR program_files = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramFiles, KF_FLAG_DEFAULT,
                                       nullptr, &program_files))) {
        const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> owned(
            program_files, CoTaskMemFree);
        return std::filesystem::path(owned.get()) / L"Llavon IME" / L"tools" /
               L"lora" / L"llavon-lora.exe";
    }
    return adjacent;
}

bool valid_revision(std::string_view value) {
    return value.size() == 40 && std::ranges::all_of(value, [](char character) {
        return character >= '0' && character <= '9' ||
               character >= 'a' && character <= 'f' ||
               character >= 'A' && character <= 'F';
    });
}

std::string read_text(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return {};
    std::string value{std::istreambuf_iterator<char>(input),
                      std::istreambuf_iterator<char>()};
    while (!value.empty() && (value.back() == '\r' || value.back() == '\n' ||
                              value.back() == ' ' || value.back() == '\t')) {
        value.pop_back();
    }
    return value;
}

std::wstring widen(std::string_view value) {
    const auto converted = utf8::utf8to16(std::string(value));
    return std::wstring(converted.begin(), converted.end());
}

std::u16string to_utf16(std::string_view value) {
    try {
        return utf8::utf8to16(std::string(value));
    } catch (...) {
        return u"LoRA process returned unreadable output";
    }
}

void throw_if_cancelled(const std::atomic_bool& cancelling) {
    if (cancelling.load(std::memory_order_acquire)) {
        throw std::runtime_error("operation cancelled");
    }
}

std::wstring quote_argument(std::wstring_view argument) {
    if (argument.empty()) return L"\"\"";
    if (argument.find_first_of(L" \t\n\v\"") == std::wstring_view::npos) {
        return std::wstring(argument);
    }
    std::wstring result = L"\"";
    std::size_t backslashes = 0;
    for (const wchar_t character : argument) {
        if (character == L'\\') {
            ++backslashes;
        } else if (character == L'\"') {
            result.append(backslashes * 2 + 1, L'\\');
            result.push_back(L'\"');
            backslashes = 0;
        } else {
            result.append(backslashes, L'\\');
            backslashes = 0;
            result.push_back(character);
        }
    }
    result.append(backslashes * 2, L'\\');
    result.push_back(L'\"');
    return result;
}

std::wstring real_argument(double value) {
    std::wostringstream output;
    output << std::setprecision(17) << value;
    return output.str();
}

std::wstring request_path(std::string_view revision, std::string_view filename) {
    return widen(std::string("/") + model_repository + "/resolve/" +
                 std::string(revision) + "/" + std::string(filename) +
                 "?download=true");
}

std::string http_get_string(const std::wstring& path,
                            WinrtHttpTransfer& transfer) {
    std::string output;
    transfer.get_stream(L"https://huggingface.co" + path,
        [&](const std::uint8_t* bytes, std::uint32_t count,
            std::uint64_t, std::uint64_t) {
            if (output.size() + count > 4 * 1024 * 1024) {
                throw std::runtime_error("Hugging Face model metadata is too large");
            }
            output.append(reinterpret_cast<const char*>(bytes), count);
        });
    return output;
}

std::string run_name() {
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    return std::to_string(milliseconds);
}

std::string utc_now() {
    return std::format("{:%FT%T}Z", std::chrono::floor<std::chrono::milliseconds>(
        std::chrono::system_clock::now()));
}

struct TrainingState {
    std::int64_t step;
};

struct ModelRevision {
    std::string sha;
};

std::int64_t training_steps(const std::filesystem::path& adapter_directory) {
    const auto state = rfl::json::read<TrainingState>(
        read_text(adapter_directory / L"training_state.json")).value();
    return state.step;
}

}  // namespace

LoraTrainingManager::LoraTrainingManager(
    std::shared_ptr<TrainingDataWriter> training_data,
    std::filesystem::path tables_directory,
    SaveCompletedModelPath save_completed_model_path)
    : training_data_(std::move(training_data)),
      tables_directory_(std::move(tables_directory)),
      save_completed_model_path_(std::move(save_completed_model_path)),
      assets_root_(environment_path(assets_path_environment).value_or(
          local_app_data_root() / L"training-assets")) {
    if (!training_data_) throw std::invalid_argument("training data dependency is required");
    std::string revision;
    status_.model_available = installed_model_is_complete(&revision);
    if (status_.model_available) {
        status_.model_revision = utf8::utf8to16(revision);
        status_.stage = LoraOperationStage::model_ready;
        status_.message = u"基礎模型已下載";
    }
}

LoraTrainingManager::~LoraTrainingManager() {
    cancel();
    std::lock_guard operation_lock(operation_mutex_);
    if (worker_.joinable()) worker_.join();
}

LoraOperationStatus LoraTrainingManager::status() {
    // The model may have appeared after the service was started (for example,
    // while an installer was replacing files). Keep the UI's cached status in
    // sync with the local files without requiring a network request.
    if (!busy_.load(std::memory_order_acquire)) {
        std::string revision;
        if (installed_model_is_complete(&revision)) {
            std::lock_guard lock(status_mutex_);
            if (!status_.model_available) {
                status_.model_available = true;
                status_.model_update_available = false;
                status_.model_revision = utf8::utf8to16(revision);
                if (status_.stage == LoraOperationStage::idle) {
                    status_.stage = LoraOperationStage::model_ready;
                    status_.progress = 1;
                    status_.message = u"基礎模型已下載";
                }
            }
        }
    }
    std::lock_guard lock(status_mutex_);
    return status_;
}

bool LoraTrainingManager::check_model_async() {
    return launch(&LoraTrainingManager::check_model_worker);
}

bool LoraTrainingManager::download_model_async() {
    return launch(&LoraTrainingManager::download_model_worker);
}

bool LoraTrainingManager::start_training_async(
    std::vector<std::u16string> event_ids,
    const std::vector<std::u16string>& reviewed_event_ids,
    LoraTrainingOptions options, std::string_view password) {
    {
        std::lock_guard operation_lock(operation_mutex_);
        if (busy_.load(std::memory_order_acquire)) return false;
        if (worker_.joinable()) worker_.join();
        auto records = training_data_->pending_records(event_ids, password);
        if (records.empty() || records.size() != event_ids.size()) return false;
        if (!training_data_->exclude_unselected(event_ids, reviewed_event_ids)) return false;
        pending_records_ = std::move(records);
        pending_event_ids_ = std::move(event_ids);
        pending_options_ = std::move(options);
        cancelling_.store(false, std::memory_order_release);
        http_transfer_.reset();
        busy_.store(true, std::memory_order_release);
        try {
            worker_ = std::thread([this] {
                try {
                    training_worker();
                } catch (const std::exception& error) {
                    set_failed(error);
                } catch (...) {
                    set_failed_unknown();
                }
                pending_records_.clear();
                busy_.store(false, std::memory_order_release);
            });
        } catch (...) {
            pending_records_.clear();
            busy_.store(false, std::memory_order_release);
            throw;
        }
    }
    return true;
}

std::size_t LoraTrainingManager::discard_plaintext_datasets() {
    std::lock_guard operation_lock(operation_mutex_);
    if (busy_.load(std::memory_order_acquire)) throw std::runtime_error("training is busy");
    return discard_plaintext_training_datasets(assets_root_ / L"runs");
}

void LoraTrainingManager::reset_conversation_data() {
    std::lock_guard operation_lock(operation_mutex_);
    if (busy_.load(std::memory_order_acquire)) throw std::runtime_error("training is busy");
    (void)discard_plaintext_training_datasets(assets_root_ / L"runs");
    training_data_->reset_conversation_data();
    pending_records_.clear();
    pending_event_ids_.clear();
}

bool LoraTrainingManager::launch(Operation operation) {
    std::lock_guard operation_lock(operation_mutex_);
    if (busy_.load(std::memory_order_acquire)) return false;
    if (worker_.joinable()) worker_.join();
    cancelling_.store(false, std::memory_order_release);
    http_transfer_.reset();
    busy_.store(true, std::memory_order_release);
    try {
        worker_ = std::thread([this, operation] {
            try {
                (this->*operation)();
            } catch (const std::exception& error) {
                set_failed(error);
            } catch (...) {
                set_failed_unknown();
            }
            busy_.store(false, std::memory_order_release);
        });
    } catch (...) {
        busy_.store(false, std::memory_order_release);
        throw;
    }
    return true;
}

void LoraTrainingManager::cancel() noexcept {
    cancelling_.store(true, std::memory_order_release);
    http_transfer_.cancel();
    std::lock_guard lock(process_mutex_);
    if (active_process_) TerminateProcess(active_process_, ERROR_CANCELLED);
}

void LoraTrainingManager::set_status(LoraOperationStage stage, double progress,
                                     std::u16string message) {
    std::lock_guard lock(status_mutex_);
    status_.stage = stage;
    status_.progress = std::clamp(progress, 0.0, 1.0);
    status_.message = std::move(message);
    if (stage != LoraOperationStage::completed) status_.output_model_path.clear();
}

void LoraTrainingManager::set_failed(const std::exception& error) noexcept {
    try {
        const bool cancelled = cancelling_.load(std::memory_order_acquire);
        auto message = cancelled ? std::u16string(u"操作已取消")
                                 : to_utf16(error.what());
        std::lock_guard lock(status_mutex_);
        status_.stage = cancelled ? LoraOperationStage::cancelled
                                  : LoraOperationStage::failed;
        status_.message = std::move(message);
    } catch (...) {
        set_failed_unknown();
    }
}

void LoraTrainingManager::set_failed_unknown() noexcept {
    try {
        const bool cancelled = cancelling_.load(std::memory_order_acquire);
        std::lock_guard lock(status_mutex_);
        status_.stage = cancelled ? LoraOperationStage::cancelled
                                  : LoraOperationStage::failed;
        status_.message = cancelled ? u"操作已取消" : u"LoRA 發生未知錯誤";
    } catch (...) {
        // This is the final thread boundary. Never terminate the service while
        // attempting to publish an error to the settings UI.
    }
}

std::string LoraTrainingManager::resolve_remote_revision() {
    const std::string body = http_get_string(
        L"/api/models/tony65535/llavon-ime-llama-250m/revision/main",
        http_transfer_);
    const auto metadata = rfl::json::read<ModelRevision>(body).value();
    const std::string revision = metadata.sha;
    if (!valid_revision(revision)) {
        throw std::runtime_error("Hugging Face returned an invalid model revision");
    }
    return revision;
}

bool LoraTrainingManager::installed_model_is_complete(std::string* revision) const {
    const std::string current = read_text(
        assets_root_ / model_repository_directory / L"current.revision");
    if (!valid_revision(current)) return false;
    const auto directory = assets_root_ / model_repository_directory / widen(current);
    std::error_code error;
    for (const auto file : model_files) {
        if (!std::filesystem::is_regular_file(directory / widen(file), error)) return false;
        error.clear();
    }
    if (revision) *revision = current;
    return true;
}

void LoraTrainingManager::check_model_worker() {
    set_status(LoraOperationStage::checking_model, 0, u"正在檢查基礎模型更新…");
    std::string local;
    const bool available = installed_model_is_complete(&local);
    {
        std::lock_guard lock(status_mutex_);
        status_.model_available = available;
        status_.model_update_available = false;
        status_.model_revision = available ? utf8::utf8to16(local) : std::u16string{};
    }
    const std::string remote = resolve_remote_revision();
    std::lock_guard lock(status_mutex_);
    status_.model_available = available;
    status_.model_update_available = !available || local != remote;
    status_.model_revision = utf8::utf8to16(available ? local : remote);
    status_.stage = available ? LoraOperationStage::model_ready
                              : LoraOperationStage::idle;
    status_.progress = available ? 1 : 0;
    status_.message = !available
        ? u"尚未下載基礎模型"
        : (local == remote ? u"基礎模型已是最新版本"
                           : u"有新的基礎模型可以下載");
}

void LoraTrainingManager::download_asset(
    const std::string& revision, std::string_view filename,
    double progress_start, double progress_end) {
    const auto directory = assets_root_ / model_repository_directory / widen(revision);
    std::filesystem::create_directories(directory);
    const auto destination = directory / widen(filename);
    const auto partial = destination.wstring() + L".partial";
    std::error_code error;
    std::filesystem::remove(partial, error);

    std::ofstream output(partial, std::ios::binary | std::ios::trunc);
    if (!output) throw std::runtime_error("unable to create model download file");
    try {
        http_transfer_.get_stream(
            L"https://huggingface.co" + request_path(revision, filename),
            [&](const std::uint8_t* bytes, std::uint32_t count,
                std::uint64_t received, std::uint64_t total) {
            output.write(reinterpret_cast<const char*>(bytes), count);
            if (!output) throw std::runtime_error("unable to write model download");
            const double fraction = total == 0 ? 0 :
                std::min(1.0, static_cast<double>(received) / static_cast<double>(total));
            set_status(LoraOperationStage::downloading_model,
                       progress_start + (progress_end - progress_start) * fraction,
                       u"正在下載 " + utf8::utf8to16(std::string(filename)));
            });
        output.flush();
        if (!output) {
            throw std::runtime_error("model download is incomplete");
        }
        output.close();
        std::filesystem::remove(destination, error);
        error.clear();
        std::filesystem::rename(partial, destination, error);
        if (error) throw std::filesystem::filesystem_error(
            "unable to publish model asset", partial, destination, error);
    } catch (...) {
        output.close();
        std::filesystem::remove(partial, error);
        throw;
    }
}

void LoraTrainingManager::download_model_worker() {
    set_status(LoraOperationStage::checking_model, 0, u"正在取得基礎模型版本…");
    const std::string revision = resolve_remote_revision();
    std::string local;
    if (installed_model_is_complete(&local) && local == revision) {
        std::lock_guard lock(status_mutex_);
        status_.stage = LoraOperationStage::model_ready;
        status_.progress = 1;
        status_.message = u"基礎模型已是最新版本";
        status_.model_available = true;
        status_.model_update_available = false;
        status_.model_revision = utf8::utf8to16(revision);
        return;
    }

    download_asset(revision, model_files[0], 0.00, 0.02);
    download_asset(revision, model_files[1], 0.02, 0.05);
    download_asset(revision, model_files[2], 0.05, 1.00);
    throw_if_cancelled(cancelling_);

    const auto root = assets_root_ / model_repository_directory;
    const auto partial = root / L"current.revision.partial";
    const auto current = root / L"current.revision";
    {
        std::ofstream output(partial, std::ios::binary | std::ios::trunc);
        output << revision << '\n';
        output.flush();
        if (!output) throw std::runtime_error("unable to store model revision");
    }
    std::error_code error;
    std::filesystem::remove(current, error);
    error.clear();
    std::filesystem::rename(partial, current, error);
    if (error) throw std::filesystem::filesystem_error(
        "unable to publish model revision", partial, current, error);

    std::lock_guard lock(status_mutex_);
    status_.stage = LoraOperationStage::model_ready;
    status_.progress = 1;
    status_.message = u"基礎模型下載完成";
    status_.model_available = true;
    status_.model_update_available = false;
    status_.model_revision = utf8::utf8to16(revision);
}

int LoraTrainingManager::run_process(
    const std::filesystem::path& executable,
    const std::vector<std::wstring>& arguments,
    bool parse_training_progress) {
    throw_if_cancelled(cancelling_);
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    HANDLE read_pipe_raw = nullptr;
    HANDLE write_pipe_raw = nullptr;
    if (!CreatePipe(&read_pipe_raw, &write_pipe_raw, &security, 0)) {
        throw std::runtime_error("unable to create trainer output pipe");
    }
    const auto close_handle = [](HANDLE handle) { if (handle) CloseHandle(handle); };
    std::unique_ptr<void, decltype(close_handle)> read_pipe(read_pipe_raw, close_handle);
    std::unique_ptr<void, decltype(close_handle)> write_pipe(write_pipe_raw, close_handle);
    SetHandleInformation(read_pipe.get(), HANDLE_FLAG_INHERIT, 0);

    std::wstring command = quote_argument(executable.wstring());
    for (const auto& argument : arguments) {
        command.push_back(L' ');
        command += quote_argument(argument);
    }
    std::vector<wchar_t> mutable_command(command.begin(), command.end());
    mutable_command.push_back(L'\0');

    STARTUPINFOW startup{sizeof(startup)};
    startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    startup.hStdOutput = write_pipe.get();
    startup.hStdError = write_pipe.get();
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION process{};
    const std::wstring working_directory = executable.parent_path().wstring();
    if (!CreateProcessW(executable.c_str(), mutable_command.data(), nullptr, nullptr,
                        TRUE, CREATE_NO_WINDOW, nullptr, working_directory.c_str(),
                        &startup, &process)) {
        throw std::runtime_error("unable to start llavon-lora.exe");
    }
    close_handle(process.hThread);
    write_pipe.reset();
    {
        std::lock_guard lock(process_mutex_);
        active_process_ = process.hProcess;
        if (cancelling_.load(std::memory_order_acquire)) {
            TerminateProcess(active_process_, ERROR_CANCELLED);
        }
    }

    std::string pending;
    std::string last_line;
    std::array<char, 4096> buffer{};
    for (;;) {
        DWORD read = 0;
        if (!ReadFile(read_pipe.get(), buffer.data(), static_cast<DWORD>(buffer.size()),
                      &read, nullptr) || read == 0) {
            break;
        }
        pending.append(buffer.data(), read);
        for (;;) {
            const auto newline = pending.find('\n');
            if (newline == std::string::npos) break;
            std::string line = pending.substr(0, newline);
            pending.erase(0, newline + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!line.empty()) last_line = line;
            if (parse_training_progress && line.starts_with("step=")) {
                const auto slash = line.find('/');
                const auto space = line.find(' ', 5);
                if (slash != std::string::npos && space != std::string::npos) {
                    try {
                        const double step = std::stod(line.substr(5, slash - 5));
                        const double total = std::stod(line.substr(slash + 1,
                            space - slash - 1));
                        if (total > 0) {
                            set_status(LoraOperationStage::training,
                                0.05 + 0.80 * std::min(1.0, step / total),
                                to_utf16(line));
                        }
                    } catch (...) {
                    }
                }
            }
        }
    }
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exit_code = ERROR_GEN_FAILURE;
    GetExitCodeProcess(process.hProcess, &exit_code);
    {
        std::lock_guard lock(process_mutex_);
        active_process_ = nullptr;
    }
    close_handle(process.hProcess);
    if (exit_code != 0) {
        throw std::runtime_error(last_line.empty()
            ? "llavon-lora.exe failed"
            : "llavon-lora.exe: " + last_line);
    }
    return static_cast<int>(exit_code);
}

void LoraTrainingManager::training_worker() {
    throw_if_cancelled(cancelling_);
    if (pending_event_ids_.empty()) {
        throw std::invalid_argument("at least one training record must be selected");
    }
    std::string revision;
    if (!installed_model_is_complete(&revision)) {
        throw std::runtime_error("base training model has not been downloaded");
    }
    const auto previous_run = training_data_->latest_lora_training_run();
    if (previous_run) {
        if (previous_run->rank != pending_options_.rank ||
            previous_run->alpha != pending_options_.alpha ||
            previous_run->dropout != pending_options_.dropout ||
            previous_run->target_modules != pending_options_.target_modules) {
            throw std::runtime_error(
                "目前參數與上一版模型不相容，無法接續訓練。");
        }
        revision = previous_run->base_model_revision;
        if (!std::filesystem::is_regular_file(
                previous_run->adapter_path / L"adapter_model.safetensors")) {
            throw std::runtime_error("previous LoRA adapter is missing");
        }
    }
    const auto trainer = trainer_executable();
    std::error_code error;
    if (!std::filesystem::is_regular_file(trainer, error)) {
        throw std::runtime_error(
            "LoRA trainer is not installed; install the optional LoRA component first");
    }

    set_status(LoraOperationStage::preparing_data, 0.01,
               u"正在由 service 轉換訓練資料…");
    const auto& records = pending_records_;
    const auto model_directory = assets_root_ / model_repository_directory / widen(revision);
    const auto run_directory = assets_root_ / L"runs" / widen(run_name());
    const auto dataset_path = run_directory / L"training.jsonl";
    struct DatasetCleanup {
        std::filesystem::path path;
        ~DatasetCleanup() {
            std::error_code ignored;
            std::filesystem::remove(path, ignored);
            path += L".partial";
            std::filesystem::remove(path, ignored);
        }
    } cleanup{dataset_path};
    const auto adapter_directory = run_directory / L"adapter";
    const auto f16_path = run_directory / L"personalized-f16.gguf";
    const auto gguf_path = run_directory / L"personalized-Q4_K_M.gguf";
    const auto config_path = model_directory / L"config.json";
    const auto vocabulary_path = model_directory / L"ime_vocab.json";
    const auto dataset = write_lora_numeric_dataset(
        records, tables_directory_, config_path, dataset_path,
        pending_options_.max_sequence_length);

    throw_if_cancelled(cancelling_);
    set_status(LoraOperationStage::training, 0.05,
               u"已準備 " + std::u16string(utf8::utf8to16(
                   std::to_string(dataset.written))) + u" 筆資料，正在啟動訓練…");
    std::vector<std::wstring> train_arguments{
        L"train",
        L"--model-config", config_path.wstring(),
        L"--model", model_directory.wstring(),
        L"--train-data", dataset_path.wstring(),
        L"--output-dir", adapter_directory.wstring(),
        L"--target-modules", std::wstring(pending_options_.target_modules.begin(),
                                           pending_options_.target_modules.end()),
        L"--pad-token-id", std::to_wstring(dataset.pad_token_id),
        L"--max-seq-length", std::to_wstring(pending_options_.max_sequence_length),
        L"--rank", std::to_wstring(pending_options_.rank),
        L"--alpha", real_argument(pending_options_.alpha),
        L"--dropout", real_argument(pending_options_.dropout),
        L"--batch-size", std::to_wstring(pending_options_.batch_size),
        L"--gradient-accumulation", std::to_wstring(pending_options_.gradient_accumulation),
        L"--epochs", std::to_wstring(pending_options_.epochs),
        L"--max-steps", std::to_wstring(pending_options_.max_steps),
        L"--learning-rate", real_argument(pending_options_.learning_rate),
        L"--weight-decay", real_argument(pending_options_.weight_decay),
        L"--warmup-steps", std::to_wstring(pending_options_.warmup_steps),
        L"--max-grad-norm", real_argument(pending_options_.max_gradient_norm),
        L"--save-every", std::to_wstring(pending_options_.save_every),
        L"--device", pending_options_.device == 2 ? L"cuda" :
                       (pending_options_.device == 1 ? L"cpu" : L"auto"),
        L"--dtype", std::wstring(pending_options_.dtype.begin(),
                                  pending_options_.dtype.end()),
        L"--seed", std::to_wstring(pending_options_.seed),
    };
    if (previous_run) {
        train_arguments.push_back(L"--resume-adapter");
        train_arguments.push_back(previous_run->adapter_path.wstring());
    }
    if (!pending_options_.shuffle) train_arguments.push_back(L"--no-shuffle");
    run_process(trainer, train_arguments, true);
    std::filesystem::remove(dataset_path);

    throw_if_cancelled(cancelling_);
    set_status(LoraOperationStage::exporting_model, 0.88,
               u"訓練完成，正在匯出並量化 GGUF…");
    const std::vector<std::wstring> export_arguments{
        L"export-gguf",
        L"--model-config", config_path.wstring(),
        L"--model", model_directory.wstring(),
        L"--vocab-file", vocabulary_path.wstring(),
        L"--adapter", adapter_directory.wstring(),
        L"--outfile", f16_path.wstring(),
        L"--outtype", L"f16",
        L"--quantize", L"Q4_K_M",
        L"--quantized-outfile", gguf_path.wstring(),
        L"--force",
    };
    run_process(trainer, export_arguments, false);
    std::filesystem::remove(f16_path, error);
    if (!std::filesystem::is_regular_file(gguf_path, error)) {
        throw std::runtime_error("trainer completed without producing a GGUF model");
    }
    const LoraTrainingRun completed_run{
        .parent_id = previous_run ? previous_run->id : 0,
        .base_model_revision = revision,
        .adapter_path = adapter_directory,
        .output_model_path = gguf_path,
        .completed_at_utc = utc_now(),
        .record_count = dataset.written,
        .cumulative_record_count = dataset.written +
            (previous_run ? previous_run->cumulative_record_count : 0),
        .optimizer_steps = training_steps(adapter_directory),
        .rank = pending_options_.rank,
        .alpha = pending_options_.alpha,
        .dropout = pending_options_.dropout,
        .target_modules = pending_options_.target_modules,
    };
    if (!training_data_->complete_lora_training(
            completed_run, dataset.included_event_ids)) {
        throw std::runtime_error("model completed but training records could not be marked trained");
    }
    if (save_completed_model_path_ &&
        !save_completed_model_path_(gguf_path)) {
        std::clog << "[SRV] unable to persist the completed model path\n";
    }

    std::lock_guard lock(status_mutex_);
    status_.stage = LoraOperationStage::completed;
    status_.progress = 1;
    status_.message.clear();
    status_.output_model_path = gguf_path.u16string();
}

}  // namespace llavon::service
