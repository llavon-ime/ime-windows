#include "lora_training_manager.hpp"
#include "training_data_cleanup.hpp"

#include "lora_dataset_builder.hpp"
#include "winrt_http.hpp"

#include <rfl/json.hpp>
#include <shlobj.h>
#include <bcrypt.h>
#include <utf8/cpp20.h>

#include <algorithm>
#include <array>
#include <map>
#include <charconv>
#include <chrono>
#include <fstream>
#include <format>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace llavon::service {
namespace {

constexpr wchar_t assets_path_environment[] = L"LLAVON_IME_LORA_ASSETS_DIR";
constexpr wchar_t trainer_path_environment[] = L"LLAVON_IME_LORA_CLI_PATH";
constexpr char trainer_commit[] = LLAVON_LORA_SUBMODULE_COMMIT;
constexpr char packaged_trainer_version[] = LLAVON_LORA_RELEASE_VERSION;
constexpr wchar_t trainer_release_root[] =
    L"https://github.com/llavon-ime/lora-trainer/releases/download/";
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

std::string read_text(const std::filesystem::path& path);
std::wstring widen(std::string_view value);

std::filesystem::path trainer_executable() {
    if (auto configured = environment_path(trainer_path_environment)) return *configured;
    const auto local = local_app_data_root() / L"tools" / L"lora";
    const auto selected = read_text(local / L"current.install");
    if (!selected.empty() && selected.find("..") == std::string::npos &&
        selected.find('\\') == std::string::npos &&
        selected.find(':') == std::string::npos &&
        selected.front() != '/' &&
        std::ranges::count(selected, '/') == 1) {
        const auto candidate = local / widen(selected) / L"llavon-lora.exe";
        std::error_code ignored;
        if (std::filesystem::is_regular_file(candidate, ignored)) return candidate;
    }
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

struct TrainerReleaseEntry {
    std::string tag_name;
    std::string target_commitish;
};

struct TrainerReleaseAsset {
    std::string name;
    std::string url;
    std::string sha256;
    std::uint64_t size;
};

struct TrainerReleaseManifest {
    std::int32_t schema;
    std::string version;
    std::int32_t trainerApi;
    std::string commit;
    std::map<std::string, TrainerReleaseAsset> assets;
};

struct InstalledTrainerManifest {
    std::int32_t schema;
    std::string version;
    std::int32_t trainerApi;
    std::string commit;
    std::string backend;
};

bool valid_calver(std::string_view value);
bool valid_https(std::string_view value);
bool valid_sha256(std::string_view value);
std::string download_text(WinrtHttpTransfer& transfer, std::wstring url);

TrainerReleaseManifest resolve_trainer_release(WinrtHttpTransfer& transfer) {
    if (!valid_revision(trainer_commit))
        throw std::runtime_error("LoRA submodule commit is unavailable in this build");
    const auto load_version = [&transfer](std::string_view version)
        -> std::optional<TrainerReleaseManifest> {
        if (!valid_calver(version)) return std::nullopt;
        const auto body = download_text(transfer,
            std::wstring(trainer_release_root) + L"v" + widen(version) +
                L"/latest.json");
        auto manifest = rfl::json::read<TrainerReleaseManifest>(body).value();
        if (manifest.schema != 1 || manifest.trainerApi != 1 ||
            manifest.commit != trainer_commit || manifest.version != version)
            return std::nullopt;
        return manifest;
    };
    if (packaged_trainer_version[0]) {
        try {
            if (auto manifest = load_version(packaged_trainer_version))
                return std::move(*manifest);
        } catch (const std::exception&) {
            // A local build can retain an older CMake cache entry. Search by
            // gitlink commit before reporting that the release is missing.
        }
    }
    for (int page = 1; page <= 10; ++page) {
        const auto body = download_text(transfer,
            L"https://api.github.com/repos/llavon-ime/lora-trainer/releases?per_page=100&page=" +
                std::to_wstring(page));
        const auto releases =
            rfl::json::read<std::vector<TrainerReleaseEntry>>(body).value();
        if (releases.empty()) break;
        for (const auto& release : releases) {
            if (release.target_commitish == trainer_commit &&
                release.tag_name.starts_with('v') &&
                valid_calver(std::string_view(release.tag_name).substr(1))) {
                if (auto manifest = load_version(
                        std::string_view(release.tag_name).substr(1)))
                    return std::move(*manifest);
            }
        }
    }
    throw std::runtime_error("No release matches the pinned LoRA submodule commit");
}

bool valid_calver(std::string_view value) {
    if (value.size() < 12 || value[4] != '.' || value[7] != '.' ||
        value[10] != '.') return false;
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (index == 4 || index == 7 || index == 10) continue;
        if (value[index] < '0' || value[index] > '9') return false;
    }
    return true;
}

bool valid_https(std::string_view value) {
    return value.starts_with("https://");
}

bool valid_sha256(std::string_view value) {
    return value.size() == 64 && std::ranges::all_of(value, [](char c) {
        return c >= '0' && c <= '9' || c >= 'a' && c <= 'f' ||
               c >= 'A' && c <= 'F';
    });
}

bool backend_matches(std::string_view backend, std::string_view asset) {
    if (asset == "win-x64-cpu") return backend == "cpu";
    if (asset == "win-x64-cuda") return backend.starts_with("cuda");
    if (asset == "win-x64-rocm") return backend.starts_with("rocm");
    return false;
}

bool valid_trainer_asset(const TrainerReleaseAsset& asset) {
    return valid_https(asset.url) && valid_sha256(asset.sha256) &&
           asset.size > 0 && std::string_view(asset.name).ends_with(".zip");
}

std::string download_text(WinrtHttpTransfer& transfer, std::wstring url) {
    std::string body;
    transfer.get_stream(std::move(url), [&](const std::uint8_t* bytes,
                                              std::uint32_t size,
                                              std::uint64_t, std::uint64_t) {
        if (body.size() + size > 4 * 1024 * 1024)
            throw std::runtime_error("LoRA release metadata is too large");
        body.append(reinterpret_cast<const char*>(bytes), size);
    });
    return body;
}

std::string sha256_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("unable to read LoRA archive");
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
        throw std::runtime_error("unable to initialize SHA-256");
    struct CloseAlgorithm {
        BCRYPT_ALG_HANDLE value;
        ~CloseAlgorithm() { BCryptCloseAlgorithmProvider(value, 0); }
    } close{algorithm};
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) < 0)
        throw std::runtime_error("unable to create SHA-256 hash");
    struct CloseHash {
        BCRYPT_HASH_HANDLE value;
        ~CloseHash() { BCryptDestroyHash(value); }
    } close_hash{hash};
    std::vector<char> buffer(1024 * 1024);
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = input.gcount();
        if (count > 0 && BCryptHashData(hash,
                reinterpret_cast<PUCHAR>(buffer.data()),
                static_cast<ULONG>(count), 0) < 0)
            throw std::runtime_error("unable to hash LoRA archive");
    }
    if (!input.eof()) throw std::runtime_error("unable to read LoRA archive completely");
    std::array<std::uint8_t, 32> digest{};
    if (BCryptFinishHash(hash, digest.data(),
                         static_cast<ULONG>(digest.size()), 0) < 0)
        throw std::runtime_error("unable to finish LoRA archive hash");
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(64);
    for (auto byte : digest) {
        result.push_back(digits[byte >> 4]);
        result.push_back(digits[byte & 15]);
    }
    return result;
}

std::filesystem::path system_tar() {
    std::wstring directory(MAX_PATH, L'\0');
    const auto length = GetSystemDirectoryW(directory.data(),
                                           static_cast<UINT>(directory.size()));
    if (length == 0 || length >= directory.size())
        throw std::runtime_error("unable to locate Windows tar.exe");
    directory.resize(length);
    return std::filesystem::path(directory) / L"tar.exe";
}

void validate_archive_listing(std::string_view listing) {
    std::istringstream lines{std::string(listing)};
    std::string entry;
    bool found = false;
    while (std::getline(lines, entry)) {
        if (!entry.empty() && entry.back() == '\r') entry.pop_back();
        if (entry.empty() || entry.front() == '/' ||
            entry.find('\\') != std::string::npos ||
            entry.find(':') != std::string::npos)
            throw std::runtime_error("LoRA archive contains an unsafe path");
        if (entry.starts_with("./")) entry.erase(0, 2);
        if (entry.empty()) continue;
        std::string_view remaining(entry);
        while (!remaining.empty()) {
            const auto separator = remaining.find('/');
            const auto part = remaining.substr(0, separator);
            if (part.empty() || part == "." || part == "..")
                throw std::runtime_error("LoRA archive contains an unsafe path");
            found = true;
            if (separator == std::string_view::npos ||
                separator + 1 == remaining.size()) break;
            remaining.remove_prefix(separator + 1);
        }
    }
    if (!found) throw std::runtime_error("LoRA archive is empty");
}

void select_trainer(const std::filesystem::path& root, std::string_view relative) {
    std::filesystem::create_directories(root);
    const auto partial = root / L"current.install.partial";
    const auto selected = root / L"current.install";
    {
        std::ofstream marker(partial, std::ios::trunc);
        marker << relative << '\n';
        marker.flush();
        if (!marker) throw std::runtime_error("unable to select installed trainer");
    }
    if (!MoveFileExW(partial.c_str(), selected.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::error_code ignored;
        std::filesystem::remove(partial, ignored);
        throw std::runtime_error("unable to publish trainer selection");
    }
}

std::int64_t training_steps(const std::filesystem::path& adapter_directory) {
    const auto state = rfl::json::read<TrainingState>(
        read_text(adapter_directory / L"training_state.json")).value();
    return state.step;
}

}  // namespace

LoraTrainingManager::LoraTrainingManager(
    std::shared_ptr<TrainingDataWriter> training_data,
    std::filesystem::path tables_directory)
    : training_data_(std::move(training_data)),
      tables_directory_(std::move(tables_directory)),
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
    refresh_installed_trainer();
}

void LoraTrainingManager::on_model_applied(
    const std::filesystem::path& model_path) const noexcept {
    prune_obsolete_gguf_models(model_path);
}

void LoraTrainingManager::prune_obsolete_gguf_models(
    const std::filesystem::path& applied_model_path) const noexcept {
    try {
        const auto history = training_data_->lora_training_history();
        if (history.size() < 2) return;

        std::error_code error;
        const auto normalize = [&](const std::filesystem::path& path) {
            return std::filesystem::absolute(path, error).lexically_normal();
        };
        const auto runs_root = normalize(assets_root_ / L"runs");
        if (error) return;
        const auto latest = normalize(history.back().output_model_path);
        if (error || !std::filesystem::is_regular_file(latest, error)) return;
        if (applied_model_path.empty()) return;
        const auto active = normalize(applied_model_path);
        if (error || active != latest) return;

        for (const auto& run : history) {
            const auto candidate = normalize(run.output_model_path);
            if (error) break;
            if (candidate == latest ||
                candidate.filename() != L"personalized-Q4_K_M.gguf" ||
                candidate.parent_path().parent_path() != runs_root) {
                continue;
            }
            std::filesystem::remove(candidate, error);
            if (error) {
                std::clog << "[SRV] unable to remove obsolete LoRA GGUF: "
                          << error.message() << '\n';
                error.clear();
            }
        }
    } catch (const std::exception& error) {
        std::clog << "[SRV] unable to prune obsolete LoRA GGUF models: "
                  << error.what() << '\n';
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
        refresh_installed_trainer();
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

bool LoraTrainingManager::check_trainer_async() {
    return launch(&LoraTrainingManager::check_trainer_worker);
}

bool LoraTrainingManager::install_trainer_async(std::int32_t backend) {
    if (backend < 0 || backend > 2) return false;
    return launch(&LoraTrainingManager::install_trainer_worker, backend);
}

void LoraTrainingManager::refresh_installed_trainer() {
    const auto executable = trainer_executable();
    std::error_code error;
    const bool exists = std::filesystem::is_regular_file(executable, error);
    std::string version;
    std::string backend;
    std::string commit;
    if (exists) {
        const auto body = read_text(executable.parent_path() /
                                    L"llavon-lora-manifest.json");
        if (!body.empty()) {
            const auto parsed = rfl::json::read<InstalledTrainerManifest>(body);
            if (parsed && parsed.value().schema == 1 &&
                parsed.value().trainerApi == 1 &&
                valid_calver(parsed.value().version)) {
                version = parsed.value().version;
                backend = parsed.value().backend;
                commit = parsed.value().commit;
            }
        }
    }
    std::lock_guard lock(status_mutex_);
    status_.trainer_available = exists;
    status_.trainer_version = to_utf16(version);
    status_.trainer_backend = to_utf16(backend);
    status_.trainer_commit = to_utf16(commit);
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

bool LoraTrainingManager::launch(Operation operation, std::int32_t trainer_backend) {
    std::lock_guard operation_lock(operation_mutex_);
    if (busy_.load(std::memory_order_acquire)) return false;
    if (worker_.joinable()) worker_.join();
    if (trainer_backend >= 0) pending_trainer_backend_ = trainer_backend;
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
    if (stage != LoraOperationStage::completed &&
        stage != LoraOperationStage::checking_trainer &&
        stage != LoraOperationStage::installing_trainer)
        status_.output_model_path.clear();
}

void LoraTrainingManager::set_failed(const std::exception& error) noexcept {
    try {
        const bool cancelled = cancelling_.load(std::memory_order_acquire);
        auto message = cancelled ? std::u16string(u"操作已取消")
                                 : to_utf16(error.what());
        std::lock_guard lock(status_mutex_);
        if (status_.stage == LoraOperationStage::checking_trainer ||
            status_.stage == LoraOperationStage::installing_trainer)
            status_.trainer_message = message;
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
        if (status_.stage == LoraOperationStage::checking_trainer ||
            status_.stage == LoraOperationStage::installing_trainer)
            status_.trainer_message = cancelled ? u"操作已取消" : u"LoRA 訓練器發生未知錯誤";
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

void LoraTrainingManager::check_trainer_worker() {
    set_status(LoraOperationStage::checking_trainer, 0,
               u"正在查詢 LoRA 訓練器發行版…");
    try {
        const auto manifest = resolve_trainer_release(http_transfer_);
        std::int32_t assets = 0;
        constexpr std::array<std::string_view, 3> names{
            "win-x64-cpu", "win-x64-cuda", "win-x64-rocm"};
        for (std::size_t index = 0; index < names.size(); ++index) {
            const auto asset = manifest.assets.find(std::string(names[index]));
            if (asset != manifest.assets.end() && valid_trainer_asset(asset->second))
                assets |= 1 << index;
        }
        std::lock_guard lock(status_mutex_);
        status_.trainer_assets = assets;
        status_.trainer_release_version = to_utf16(manifest.version);
        status_.trainer_message =
            status_.trainer_version == status_.trainer_release_version &&
            status_.trainer_commit == to_utf16(manifest.commit)
                ? u"已安裝此 submodule 對應的發行版"
                : u"可下載此 submodule 對應的發行版";
        status_.stage = !status_.output_model_path.empty()
            ? LoraOperationStage::completed
            : (status_.model_available ? LoraOperationStage::model_ready
                                       : LoraOperationStage::idle);
        status_.message = status_.trainer_message;
    } catch (const std::exception& error) {
        std::lock_guard lock(status_mutex_);
        status_.trainer_assets = 0;
        status_.trainer_release_version.clear();
        status_.trainer_message = cancelling_.load(std::memory_order_acquire)
            ? u"查詢已取消" : to_utf16(error.what());
        status_.stage = !status_.output_model_path.empty()
            ? LoraOperationStage::completed
            : (status_.model_available ? LoraOperationStage::model_ready
                                       : LoraOperationStage::idle);
        status_.message = status_.trainer_message;
    }
}

void LoraTrainingManager::install_trainer_worker() {
    set_status(LoraOperationStage::checking_trainer, 0,
               u"正在查詢 LoRA 訓練器版本…");
    const auto manifest = resolve_trainer_release(http_transfer_);
    constexpr std::array<std::string_view, 3> asset_names{
        "win-x64-cpu", "win-x64-cuda", "win-x64-rocm"};
    const auto asset_name = asset_names.at(
        static_cast<std::size_t>(pending_trainer_backend_));
    const auto asset = manifest.assets.find(std::string(asset_name));
    if (asset == manifest.assets.end())
        throw std::runtime_error("This release has no Windows asset for the selected backend");
    if (!valid_trainer_asset(asset->second))
        throw std::runtime_error("LoRA trainer asset metadata is invalid");

    const auto root = local_app_data_root() / L"tools" / L"lora";
    const auto relative = manifest.version + "/" + std::string(asset_name);
    const auto destination = root / widen(relative);
    const auto matches_release = [&](const std::filesystem::path& directory) {
        const auto body = read_text(directory / L"llavon-lora-manifest.json");
        const auto parsed = rfl::json::read<InstalledTrainerManifest>(body);
        std::error_code ignored;
        return parsed && parsed.value().version == manifest.version &&
               parsed.value().trainerApi == manifest.trainerApi &&
               parsed.value().commit == manifest.commit &&
               backend_matches(parsed.value().backend, asset_name) &&
               std::filesystem::is_regular_file(
                   directory / L"llavon-lora.exe", ignored);
    };
    if (matches_release(destination)) {
        select_trainer(root, relative);
        refresh_installed_trainer();
        std::lock_guard lock(status_mutex_);
        status_.trainer_release_version = to_utf16(manifest.version);
        status_.trainer_message = u"LoRA 訓練器已安裝";
        status_.stage = !status_.output_model_path.empty()
            ? LoraOperationStage::completed
            : (status_.model_available ? LoraOperationStage::model_ready
                                       : LoraOperationStage::idle);
        status_.message = status_.trainer_message;
        return;
    }

    const auto downloads = local_app_data_root() / L"downloads";
    std::filesystem::create_directories(downloads);
    const auto archive = downloads /
        (L"llavon-lora-" + widen(run_name()) + L".zip");
    const auto staging = root /
        (L".staging-" + widen(run_name()) + L"-" +
         std::to_wstring(GetCurrentProcessId()));
    const auto backup = root /
        (L".backup-" + widen(run_name()) + L"-" +
         std::to_wstring(GetCurrentProcessId()));
    struct Cleanup {
        std::filesystem::path archive;
        std::filesystem::path staging;
        ~Cleanup() {
            std::error_code ignored;
            std::filesystem::remove(archive, ignored);
            std::filesystem::remove_all(staging, ignored);
        }
    } cleanup{archive, staging};
    std::ofstream output(archive, std::ios::binary | std::ios::trunc);
    if (!output) throw std::runtime_error("unable to save LoRA archive");
    std::uint64_t received = 0;
    http_transfer_.get_stream(widen(asset->second.url),
        [&](const std::uint8_t* bytes, std::uint32_t count,
            std::uint64_t current, std::uint64_t) {
            if (current > asset->second.size)
                throw std::runtime_error("LoRA archive exceeds the manifest size");
            output.write(reinterpret_cast<const char*>(bytes), count);
            if (!output) throw std::runtime_error("unable to write LoRA archive");
            received = current;
            set_status(LoraOperationStage::installing_trainer,
                0.8 * static_cast<double>(current) /
                    static_cast<double>(asset->second.size),
                u"正在下載 LoRA 訓練器…");
        });
    output.close();
    throw_if_cancelled(cancelling_);
    if (received != asset->second.size ||
        sha256_file(archive) != asset->second.sha256)
        throw std::runtime_error("LoRA archive SHA-256 or size mismatch");

    set_status(LoraOperationStage::installing_trainer, 0.85,
               u"正在解壓縮 LoRA 訓練器…");
    std::string listing;
    run_process(system_tar(), {L"-tf", archive.wstring()}, false, &listing);
    validate_archive_listing(listing);
    std::filesystem::create_directories(staging);
    run_process(system_tar(),
                {L"-xf", archive.wstring(), L"-C", staging.wstring()}, false);
    throw_if_cancelled(cancelling_);
    for (const auto& entry : std::filesystem::recursive_directory_iterator(staging)) {
        const auto attributes = GetFileAttributesW(entry.path().c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES ||
            (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
            throw std::runtime_error("LoRA archive contains a reparse point");
    }
    if (!matches_release(staging))
        throw std::runtime_error("extracted LoRA trainer did not match the release");
    std::filesystem::create_directories(destination.parent_path());
    const bool had_previous = std::filesystem::exists(destination);
    if (had_previous) std::filesystem::rename(destination, backup);
    try {
        std::filesystem::rename(staging, destination);
        select_trainer(root, relative);
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove_all(destination, ignored);
        if (had_previous) {
            std::filesystem::rename(backup, destination, ignored);
        }
        throw;
    }
    if (had_previous) {
        std::error_code ignored;
        std::filesystem::remove_all(backup, ignored);
    }
    refresh_installed_trainer();
    std::lock_guard lock(status_mutex_);
    status_.trainer_release_version = to_utf16(manifest.version);
    status_.trainer_message = u"LoRA 訓練器安裝完成";
    status_.stage = !status_.output_model_path.empty()
        ? LoraOperationStage::completed
        : (status_.model_available ? LoraOperationStage::model_ready
                                   : LoraOperationStage::idle);
    status_.message = status_.trainer_message;
}
int LoraTrainingManager::run_process(
    const std::filesystem::path& executable,
    const std::vector<std::wstring>& arguments,
    bool parse_training_progress,
    std::string* captured_output) {
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
    bool output_too_large = false;
    std::array<char, 4096> buffer{};
    for (;;) {
        DWORD read = 0;
        if (!ReadFile(read_pipe.get(), buffer.data(), static_cast<DWORD>(buffer.size()),
                      &read, nullptr) || read == 0) {
            break;
        }
        if (captured_output && !output_too_large) {
            if (captured_output->size() + read > 4 * 1024 * 1024)
                output_too_large = true;
            else
                captured_output->append(buffer.data(), read);
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
    if (output_too_large)
        throw std::runtime_error("LoRA archive listing is too large");
    if (exit_code != 0) {
        throw std::runtime_error(last_line.empty()
            ? "LoRA process failed"
            : "LoRA process: " + last_line);
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
    std::lock_guard lock(status_mutex_);
    status_.stage = LoraOperationStage::completed;
    status_.progress = 1;
    status_.message.clear();
    status_.output_model_path = gguf_path.u16string();
}

}  // namespace llavon::service
