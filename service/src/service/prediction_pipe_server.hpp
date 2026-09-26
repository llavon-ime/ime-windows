#pragma once

#if defined(_WIN32)

#include <windows.h>
#include <sddl.h>
#include <winrt/Windows.Foundation.h>

#include <asio.hpp>
#include <ime-core/core.hpp>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <format>
#include <future>
#include <iostream>
#include <list>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "candidate_pipe_server.hpp"
#include "custom_name_matcher.hpp"
#include "gpu_latency_boost.hpp"
#include "training_data_writer.hpp"

namespace llavon::service {

namespace prediction_pipe {

inline constexpr const wchar_t* pipe_name = L"\\\\.\\pipe\\llavon-ime";

enum class PipeCommand : uint8_t {
    Predict = 1,
    ToggleInputMode = 2,
    GetInputMode = 3,
    Ready = 4,
    RecordCommit = 5,
    DiscardLastCommit = 6,
};

enum class InputMode : uint8_t {
    Chinese = 0,
    English = 1,
};

enum class ServicePriority {
    Normal,
    AboveNormal,
    High,
    Realtime,
};

inline std::atomic<InputMode> g_input_mode{InputMode::Chinese};

inline InputMode current_input_mode() {
    return g_input_mode.load(std::memory_order_relaxed);
}

inline InputMode toggle_input_mode() {
    InputMode current = current_input_mode();
    while (true) {
        const InputMode next = current == InputMode::Chinese ? InputMode::English : InputMode::Chinese;
        if (g_input_mode.compare_exchange_weak(current, next, std::memory_order_relaxed)) {
            return next;
        }
    }
}

inline ServicePriority selected_service_priority() {
    char env[32]{};
    const DWORD len = GetEnvironmentVariableA("LLAVON_IME_SERVICE_PRIORITY", env, static_cast<DWORD>(sizeof(env)));
    if (len == 0 || len >= sizeof(env)) {
        return ServicePriority::High;
    }

    const std::string_view value(env, len);
    if (value == "normal") {
        return ServicePriority::Normal;
    }
    if (value == "above_normal" || value == "above-normal") {
        return ServicePriority::AboveNormal;
    }
    if (value == "realtime" || value == "real-time") {
        return ServicePriority::Realtime;
    }
    return ServicePriority::High;
}

inline const char* priority_name(ServicePriority priority) {
    switch (priority) {
        case ServicePriority::Normal:
            return "normal";
        case ServicePriority::AboveNormal:
            return "above_normal";
        case ServicePriority::Realtime:
            return "realtime";
        case ServicePriority::High:
        default:
            return "high";
    }
}

inline void configure_windows_scheduling(ServicePriority priority) {
    DWORD priority_class = HIGH_PRIORITY_CLASS;
    int thread_priority = THREAD_PRIORITY_HIGHEST;

    switch (priority) {
        case ServicePriority::Normal:
            priority_class = NORMAL_PRIORITY_CLASS;
            thread_priority = THREAD_PRIORITY_NORMAL;
            break;
        case ServicePriority::AboveNormal:
            priority_class = ABOVE_NORMAL_PRIORITY_CLASS;
            thread_priority = THREAD_PRIORITY_ABOVE_NORMAL;
            break;
        case ServicePriority::Realtime:
            priority_class = REALTIME_PRIORITY_CLASS;
            thread_priority = THREAD_PRIORITY_TIME_CRITICAL;
            break;
        case ServicePriority::High:
        default:
            priority_class = HIGH_PRIORITY_CLASS;
            thread_priority = THREAD_PRIORITY_HIGHEST;
            break;
    }

    if (!SetPriorityClass(GetCurrentProcess(), priority_class)) {
        std::cerr << "[WARN] SetPriorityClass failed: " << GetLastError() << std::endl;
    }
    if (!SetThreadPriority(GetCurrentThread(), thread_priority)) {
        std::cerr << "[WARN] SetThreadPriority failed: " << GetLastError() << std::endl;
    }

    PROCESS_POWER_THROTTLING_STATE throttling{};
    throttling.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    throttling.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
    throttling.StateMask = 0;
    if (!SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &throttling, sizeof(throttling))) {
        std::cerr << "[WARN] disabling process power throttling failed: " << GetLastError() << std::endl;
    }
}

inline asio::awaitable<bool> read_exact(asio::windows::stream_handle& pipe, void* buf, size_t size) {
    size_t total = 0;
    while (total < size) {
        auto [ec, n] = co_await pipe.async_read_some(
            asio::buffer(static_cast<char*>(buf) + total, size - total), asio::as_tuple(asio::use_awaitable));
        if (ec || n == 0) co_return false;
        total += n;
    }
    co_return true;
}

template <typename T>
inline asio::awaitable<bool> read_val(asio::windows::stream_handle& pipe, T& val) {
    co_return co_await read_exact(pipe, &val, sizeof(T));
}

inline asio::awaitable<bool> write_exact(asio::windows::stream_handle& pipe, const void* buf, size_t size) {
    size_t total = 0;
    while (total < size) {
        auto [ec, n] = co_await pipe.async_write_some(
            asio::buffer(static_cast<const char*>(buf) + total, size - total), asio::as_tuple(asio::use_awaitable));
        if (ec) co_return false;
        total += n;
    }
    co_return true;
}

inline asio::awaitable<bool> read_utf16_string(
    asio::windows::stream_handle& pipe, std::u16string& value,
    std::uint32_t maximum_length) {
    std::uint32_t length = 0;
    if (!co_await read_val(pipe, length) || length > maximum_length) co_return false;
    value.resize(length);
    if (length > 0 &&
        !co_await read_exact(pipe, value.data(), length * sizeof(char16_t))) {
        co_return false;
    }
    co_return true;
}

inline std::string new_collection_session_id() {
    try {
        const auto value = winrt::Windows::Foundation::GuidHelper::CreateNewGuid();
        return std::format(
            "{:08x}-{:04x}-{:04x}-{:02x}{:02x}-{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}",
            value.Data1, value.Data2, value.Data3, value.Data4[0], value.Data4[1],
            value.Data4[2], value.Data4[3], value.Data4[4], value.Data4[5],
            value.Data4[6], value.Data4[7]);
    } catch (const winrt::hresult_error&) {
        return std::format("{}-{}", GetCurrentProcessId(), GetTickCount64());
    }
}

inline std::string utc_timestamp() {
    const auto now = std::chrono::floor<std::chrono::milliseconds>(
        std::chrono::system_clock::now());
    return std::format("{:%FT%T}Z", now);
}

template <typename T>
inline asio::awaitable<bool> write_val(asio::windows::stream_handle& pipe, const T& val) {
    co_return co_await write_exact(pipe, &val, sizeof(T));
}

inline asio::awaitable<std::vector<llavon::ime::core::PaddingEntry>> read_padding(
    asio::windows::stream_handle& pipe) {
    uint32_t count = 0;
    if (!co_await read_val(pipe, count)) co_return std::vector<llavon::ime::core::PaddingEntry>{};

    std::vector<llavon::ime::core::PaddingEntry> entries;
    entries.reserve(count);
    for (uint32_t i = 0; i < count; i++) {
        uint8_t type = 0;
        if (!co_await read_val(pipe, type)) co_return std::vector<llavon::ime::core::PaddingEntry>{};

        llavon::ime::core::PaddingEntry e;
        if (type == 0) {
            uint32_t len = 0;
            if (!co_await read_val(pipe, len)) co_return std::vector<llavon::ime::core::PaddingEntry>{};
            e.bopomofo.resize(len);
            if (len > 0 && !co_await read_exact(pipe, e.bopomofo.data(), len * sizeof(char16_t))) {
                co_return std::vector<llavon::ime::core::PaddingEntry>{};
            }
            e.chosen = false;
        } else {
            if (!co_await read_val(pipe, e.chosen_char)) {
                co_return std::vector<llavon::ime::core::PaddingEntry>{};
            }
            e.chosen = true;
        }
        entries.push_back(std::move(e));
    }
    co_return entries;
}

inline asio::awaitable<void> write_response(asio::windows::stream_handle& pipe,
                                            const std::vector<llavon::ime::core::Prediction>& results) {
    uint32_t count = static_cast<uint32_t>(results.size());
    if (!co_await write_val(pipe, count)) co_return;

    for (auto& r : results) {
        uint32_t nc = static_cast<uint32_t>(r.candidates.size());
        if (!co_await write_val(pipe, nc)) co_return;
        for (auto& [c, _] : r.candidates) {
            if (!co_await write_val(pipe, c)) co_return;
        }
    }
}

inline asio::awaitable<void> write_input_mode(asio::windows::stream_handle& pipe, InputMode mode) {
    const uint8_t raw_mode = static_cast<uint8_t>(mode);
    co_await write_val(pipe, raw_mode);
}

class SessionLru final {
public:
    using ClientId = std::uint64_t;
    static constexpr std::size_t capacity = 5;

    SessionLru(asio::io_context& context, std::shared_ptr<llavon::ime::core::Core> core,
               bool gpu_boost_enabled)
        : core_(std::move(core)), gpu_activity_(context),
          gpu_boost_enabled_(gpu_boost_enabled) {
        if (!core_) {
            throw std::invalid_argument("inference core is required");
        }
        gpu_activity_.set_enabled(gpu_boost_enabled_);
        if (gpu_boost_enabled_) {
            gpu_activity_.set_backend(make_gpu_latency_boost(core_->inference_runtime_info()));
            boost_backend_initialized_ = true;
        }
    }

    llavon::ime::core::InferenceRuntimeInfo replace_core(
        llavon::ime::core::CoreConfig config) {
        auto replacement =
            std::make_shared<llavon::ime::core::Core>(std::move(config));
        sessions_.clear();
        idle_.clear();
        recency_.clear();
        core_ = std::move(replacement);
        // Release the previous ADLX runtime before initializing its replacement.
        gpu_activity_.set_backend({});
        gpu_activity_.set_backend(gpu_boost_enabled_
            ? make_gpu_latency_boost(core_->inference_runtime_info())
            : GpuActivityLease::Boost{});
        boost_backend_initialized_ = gpu_boost_enabled_;
        return core_->inference_runtime_info();
    }

    void set_gpu_boost_enabled(bool enabled) {
        if (enabled && !boost_backend_initialized_) {
            gpu_activity_.set_backend(make_gpu_latency_boost(core_->inference_runtime_info()));
            boost_backend_initialized_ = true;
        }
        gpu_boost_enabled_ = enabled;
        gpu_activity_.set_enabled(enabled);
    }

    llavon::ime::core::Session& acquire(ClientId client_id) {
        if (!core_) {
            throw std::runtime_error("inference core is not loaded");
        }
        gpu_activity_.activate();
        const auto existing = sessions_.find(client_id);
        if (existing != sessions_.end()) {
            recency_.splice(recency_.begin(), recency_, existing->second.recency);
            return *existing->second.session;
        }

        std::unique_ptr<llavon::ime::core::Session> session;
        if (!idle_.empty()) {
            session = std::move(idle_.back());
            idle_.pop_back();
        } else if (sessions_.size() < capacity) {
            session = core_->create_session();
        } else {
            const ClientId evicted_client_id = recency_.back();
            recency_.pop_back();
            auto evicted = sessions_.find(evicted_client_id);
            session = std::move(evicted->second.session);
            sessions_.erase(evicted);
            std::clog << "[SRV] reassigned LRU inference context from client="
                      << evicted_client_id << " to client=" << client_id << '\n';
        }

        recency_.push_front(client_id);
        const auto [inserted, ok] = sessions_.emplace(
            client_id, Entry{std::move(session), recency_.begin()});
        if (!ok) {
            recency_.pop_front();
            throw std::logic_error("failed to insert inference context into LRU");
        }
        return *inserted->second.session;
    }

    void erase(ClientId client_id) noexcept {
        const auto existing = sessions_.find(client_id);
        if (existing == sessions_.end()) {
            return;
        }
        recency_.erase(existing->second.recency);
        auto session = std::move(existing->second.session);
        sessions_.erase(existing);
        idle_.push_back(std::move(session));
    }

    void release_gpu_activity() noexcept {
        gpu_activity_.set_backend({});
    }

private:
    struct Entry {
        std::unique_ptr<llavon::ime::core::Session> session;
        std::list<ClientId>::iterator recency;
    };

    // SessionLru is confined to the prediction server's single io_context thread.
    std::shared_ptr<llavon::ime::core::Core> core_;
    GpuActivityLease gpu_activity_;
    bool gpu_boost_enabled_ = true;
    bool boost_backend_initialized_ = false;
    std::list<ClientId> recency_;
    std::unordered_map<ClientId, Entry> sessions_;
    std::vector<std::unique_ptr<llavon::ime::core::Session>> idle_;
};

class ClientSession final {
public:
    ClientSession(std::shared_ptr<SessionLru> sessions, SessionLru::ClientId client_id)
        : sessions_(std::move(sessions)), client_id_(client_id) {}

    ~ClientSession() {
        sessions_->erase(client_id_);
    }

    llavon::ime::core::Session& acquire() {
        return sessions_->acquire(client_id_);
    }

private:
    std::shared_ptr<SessionLru> sessions_;
    SessionLru::ClientId client_id_;
};

inline asio::awaitable<void> handle_client(
    asio::windows::stream_handle pipe,
    std::shared_ptr<SessionLru> sessions,
    std::shared_ptr<CustomNameMatcher> custom_names,
    std::shared_ptr<TrainingDataWriter> training_data,
    std::string collection_session_id,
    SessionLru::ClientId client_id) {
    ClientSession client_session(std::move(sessions), client_id);
    std::uint64_t latest_commit_token = 0;

    while (true) {
        uint8_t raw_command = 0;
        if (!co_await read_val(pipe, raw_command)) break;

        const auto command = static_cast<PipeCommand>(raw_command);
        if (command == PipeCommand::ToggleInputMode) {
            const InputMode mode = toggle_input_mode();
            std::clog << "[SRV] input mode: " << (mode == InputMode::Chinese ? "Chinese" : "English") << '\n';
            co_await write_input_mode(pipe, mode);
            continue;
        }

        if (command == PipeCommand::GetInputMode) {
            co_await write_input_mode(pipe, current_input_mode());
            continue;
        }

        if (command == PipeCommand::Ready) {
            uint8_t ok = 0;
            try {
                client_session.acquire().ready();
                ok = 1;
            } catch (const std::exception& e) {
                std::cerr << "[ERR] ready: " << e.what() << std::endl;
            }
            co_await write_val(pipe, ok);
            continue;
        }

        if (command == PipeCommand::RecordCommit) {
            constexpr std::uint32_t maximum_context_length = 4096;
            constexpr std::uint32_t maximum_answer_length = 1024;
            constexpr std::uint32_t maximum_entry_count = 1024;
            constexpr std::uint32_t maximum_entry_length = 64;
            RawCommitEvent event;
            std::uint64_t commit_token = 0;
            if (!co_await read_val(pipe, commit_token) || commit_token == 0 ||
                commit_token <= latest_commit_token ||
                !co_await read_utf16_string(
                    pipe, event.context, maximum_context_length) ||
                !co_await read_utf16_string(
                    pipe, event.answer, maximum_answer_length)) {
                break;
            }

            std::uint32_t count = 0;
            if (!co_await read_val(pipe, count) || count == 0 ||
                count > maximum_entry_count) {
                break;
            }
            event.input.reserve(count);
            bool valid = true;
            for (std::uint32_t index = 0; index < count; ++index) {
                RawCommitInputEntry entry;
                std::uint8_t manually_selected = 0;
                if (!co_await read_utf16_string(
                        pipe, entry.reading, maximum_entry_length) ||
                    !co_await read_utf16_string(
                        pipe, entry.output, maximum_entry_length) ||
                    entry.output.empty() ||
                    !co_await read_val(pipe, manually_selected) ||
                    manually_selected > 1) {
                    valid = false;
                    break;
                }
                entry.manually_selected = manually_selected != 0;
                event.input.push_back(std::move(entry));
            }
            if (!valid) break;

            event.session_id = collection_session_id;
            event.sequence = commit_token;
            event.committed_at_utc = utc_timestamp();
            training_data->enqueue(std::move(event));
            latest_commit_token = commit_token;
            continue;
        }

        if (command == PipeCommand::DiscardLastCommit) {
            std::uint64_t commit_token = 0;
            if (!co_await read_val(pipe, commit_token)) break;
            if (commit_token == latest_commit_token) {
                training_data->discard_staged(
                    collection_session_id, commit_token);
            }
            continue;
        }

        if (command != PipeCommand::Predict) {
            std::cerr << "[ERR] unknown pipe command: " << static_cast<int>(raw_command) << std::endl;
            break;
        }

        uint32_t ctx_len = 0;
        if (!co_await read_val(pipe, ctx_len)) break;

        std::u16string context;
        if (ctx_len > 0) {
            context.resize(ctx_len);
            if (!co_await read_exact(pipe, context.data(), ctx_len * sizeof(char16_t))) break;
        }

        auto padding = co_await read_padding(pipe);
        if (padding.empty() && ctx_len == 0) break;
        const auto custom_name_replacements = custom_names->apply(padding);

        std::vector<llavon::ime::core::Prediction> results;
        bool ok = false;
        try {
            results = client_session.acquire().predict(context, padding);
            for (std::size_t index = 0;
                 index < results.size() && index < custom_name_replacements.size(); ++index) {
                if (custom_name_replacements[index]) {
                    results[index].candidates = {
                        {*custom_name_replacements[index], 1.0F},
                    };
                }
            }
            ok = true;
        } catch (const std::exception& e) {
            std::cerr << "[ERR] predict: " << e.what() << std::endl;
        }
        if (ok) {
            co_await write_response(pipe, results);
        } else {
            uint32_t zero = 0;
            co_await write_val(pipe, zero);
        }
    }
}

inline asio::awaitable<void> listener(
    asio::io_context& io_ctx,
    std::shared_ptr<SessionLru> sessions,
    std::shared_ptr<CustomNameMatcher> custom_names,
    std::shared_ptr<TrainingDataWriter> training_data) {
    auto executor = co_await asio::this_coro::executor;
    SessionLru::ClientId next_client_id = 1;

    while (true) {
        // SearchHost and other modern Windows text controls run in an
        // AppContainer.  The default pipe DACL does not grant AppContainer
        // tokens access, so those hosts otherwise receive ERROR_ACCESS_DENIED
        // and block their TSF UI thread while the frontend retries.
        PSECURITY_DESCRIPTOR security_descriptor = nullptr;
        constexpr wchar_t pipe_sddl[] =
            L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;IU)(A;;GRGW;;;AC)(A;;GRGW;;;S-1-15-2-2)";
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
                pipe_sddl, SDDL_REVISION_1, &security_descriptor, nullptr)) {
            std::cerr << "[ERR] pipe security descriptor failed: " << GetLastError() << std::endl;
            co_return;
        }
        SECURITY_ATTRIBUTES security_attributes{};
        security_attributes.nLength = sizeof(security_attributes);
        security_attributes.lpSecurityDescriptor = security_descriptor;

        HANDLE hPipe = CreateNamedPipeW(
            pipe_name, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
            PIPE_UNLIMITED_INSTANCES, 65536, 65536, 0, &security_attributes);
        LocalFree(security_descriptor);

        if (hPipe == INVALID_HANDLE_VALUE) {
            std::cerr << "[ERR] CreateNamedPipe failed: " << GetLastError() << std::endl;
            co_return;
        }

        OVERLAPPED ol{};
        ol.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);

        BOOL connected = ConnectNamedPipe(hPipe, &ol);
        DWORD err = GetLastError();

        if (!connected && err == ERROR_PIPE_CONNECTED) {
            CloseHandle(ol.hEvent);
        } else if (!connected && err == ERROR_IO_PENDING) {
            asio::windows::object_handle ev(executor);
            ev.assign(ol.hEvent);
            co_await ev.async_wait(asio::use_awaitable);
        } else {
            std::cerr << "[ERR] ConnectNamedPipe failed: " << err << std::endl;
            CloseHandle(ol.hEvent);
            CloseHandle(hPipe);
            continue;
        }

        std::clog << "[SRV] client connected\n";

        asio::windows::stream_handle stream(executor, hPipe);
        const SessionLru::ClientId client_id = next_client_id++;
        co_spawn(executor,
                 handle_client(std::move(stream), sessions, custom_names,
                               training_data, new_collection_session_id(), client_id),
                 asio::detached);
    }
}

}  // namespace prediction_pipe

class PredictionPipeServer final {
public:
    PredictionPipeServer(
        std::shared_ptr<llavon::ime::core::Core> core,
        CandidateUiLoader& candidate_ui,
        std::shared_ptr<CustomNameMatcher> custom_names,
        bool gpu_boost_enabled = true)
        : sessions_(std::make_shared<prediction_pipe::SessionLru>(
              io_ctx_, std::move(core), gpu_boost_enabled)),
          candidate_ui_(candidate_ui),
          custom_names_(std::move(custom_names)),
          training_data_(std::make_shared<TrainingDataWriter>()) {
        if (!custom_names_) {
            throw std::invalid_argument("prediction server dependencies are required");
        }
    }

    const char* name() const {
        return "prediction-pipe";
    }

    llavon::ime::core::InferenceRuntimeInfo replace_core(
        llavon::ime::core::CoreConfig config) {
        if (!accepting_reloads_.load(std::memory_order_acquire)) {
            throw std::runtime_error("prediction server is not accepting model reloads");
        }

        auto completion =
            std::make_shared<std::promise<llavon::ime::core::InferenceRuntimeInfo>>();
        auto result = completion->get_future();
        asio::post(io_ctx_,
                   [sessions = sessions_, config = std::move(config), completion]() mutable {
                       try {
                           completion->set_value(sessions->replace_core(std::move(config)));
                       } catch (...) {
                           completion->set_exception(std::current_exception());
                       }
                   });
        while (result.wait_for(std::chrono::milliseconds(100)) != std::future_status::ready) {
            if (!accepting_reloads_.load(std::memory_order_acquire)) {
                throw std::runtime_error("prediction server stopped during model reload");
            }
        }
        return result.get();
    }

    void set_gpu_boost_enabled(bool enabled) {
        asio::post(io_ctx_, [sessions = sessions_, enabled] {
            sessions->set_gpu_boost_enabled(enabled);
        });
    }

    std::vector<TrainingDataItem> pending_training_data() const {
        return training_data_->pending_items();
    }


    bool exclude_unselected_training_data(
        const std::vector<std::u16string>& selected_event_ids,
        const std::vector<std::u16string>& reviewed_event_ids) noexcept {
        return training_data_->exclude_unselected(
            selected_event_ids, reviewed_event_ids);
    }

    std::shared_ptr<TrainingDataWriter> training_data_writer() const noexcept {
        return training_data_;
    }

    void stop() noexcept {
        accepting_reloads_.store(false, std::memory_order_release);
        io_ctx_.stop();
    }

    int run() {
        SetConsoleOutputCP(65001);
        const auto priority = prediction_pipe::selected_service_priority();
        prediction_pipe::configure_windows_scheduling(priority);
        std::clog << "[SRV] service priority: " << prediction_pipe::priority_name(priority) << '\n';
        std::clog << "[SRV] engine backend: llama\n";

        std::clog << "[SRV] model loaded\n";

        CandidatePipeServer candidate_pipe(candidate_ui_);
        co_spawn(io_ctx_,
                 prediction_pipe::listener(
                     io_ctx_, sessions_, custom_names_, training_data_),
                 asio::detached);
        co_spawn(io_ctx_, candidate_pipe.listen(), asio::detached);
        io_ctx_.run();
        // Release on the inference thread even when stop() prevented the idle
        // timer from running and outstanding clients still retain sessions_.
        sessions_->release_gpu_activity();
        accepting_reloads_.store(false, std::memory_order_release);

        return 0;
    }

private:
    asio::io_context io_ctx_;
    std::shared_ptr<prediction_pipe::SessionLru> sessions_;
    CandidateUiLoader& candidate_ui_;
    std::shared_ptr<CustomNameMatcher> custom_names_;
    std::shared_ptr<TrainingDataWriter> training_data_;
    std::atomic<bool> accepting_reloads_{true};
};

}  // namespace llavon::service

#endif
