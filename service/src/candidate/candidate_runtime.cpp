#include "candidate_ui_api.h"

#include "candidate_window.hpp"

#include <windows.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <winrt/base.h>

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace llavon::candidate {
namespace {

constexpr wchar_t command_window_class[] = L"LlavonImeCandidateUiCommandWindow";
constexpr UINT present_message = WM_APP + 1;
constexpr UINT hide_message = WM_APP + 2;
constexpr UINT stop_message = WM_APP + 3;
constexpr DWORD shutdown_timeout_ms = 10000;
constexpr uint32_t maximum_candidate_count = 36;
constexpr uint32_t maximum_candidate_length = 256;
constexpr uint32_t maximum_layout_columns = 4;

struct Presentation final {
    HWND owner_window = nullptr;
    int anchor_x = 0;
    int anchor_y = 0;
    int anchor_top = 0;
    std::vector<std::wstring> candidates;
    uint32_t selection_index = 0;
    uint32_t layout_columns = 1;
    uint32_t number_column = 0;
    bool can_prev_page = false;
    bool can_next_page = false;
};

class Runtime final {
public:
    int32_t start() {
        std::lock_guard lock(lifecycle_mutex_);
        if (thread_) {
            return 0;
        }

        ready_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!ready_event_) {
            return static_cast<int32_t>(GetLastError());
        }

        start_result_.store(ERROR_GEN_FAILURE, std::memory_order_relaxed);
        thread_ = CreateThread(nullptr, 0, thread_entry, this, 0, nullptr);
        if (!thread_) {
            const DWORD error = GetLastError();
            CloseHandle(ready_event_);
            ready_event_ = nullptr;
            return static_cast<int32_t>(error);
        }

        const DWORD wait_result = WaitForSingleObject(ready_event_, INFINITE);
        CloseHandle(ready_event_);
        ready_event_ = nullptr;
        if (wait_result != WAIT_OBJECT_0) {
            return static_cast<int32_t>(GetLastError());
        }

        const int32_t result = start_result_.load(std::memory_order_relaxed);
        if (result != 0) {
            WaitForSingleObject(thread_, shutdown_timeout_ms);
            CloseHandle(thread_);
            thread_ = nullptr;
        }
        return result;
    }

    int32_t present(const llavon_candidate_ui_presentation* source) {
        Presentation presentation;
        const int32_t copy_result = copy_presentation(source, presentation);
        if (copy_result != 0) {
            return copy_result;
        }

        {
            std::lock_guard lock(presentation_mutex_);
            pending_presentation_ = std::move(presentation);
        }
        return post(present_message) ? 0 : static_cast<int32_t>(GetLastError());
    }

    void hide() noexcept {
        {
            std::lock_guard lock(presentation_mutex_);
            pending_presentation_.reset();
        }
        post(hide_message);
    }

    int32_t stop() {
        std::lock_guard lock(lifecycle_mutex_);
        if (!thread_) {
            return 0;
        }

        {
            std::lock_guard presentation_lock(presentation_mutex_);
            pending_presentation_.reset();
        }
        post(stop_message);

        const DWORD wait_result = WaitForSingleObject(thread_, shutdown_timeout_ms);
        if (wait_result != WAIT_OBJECT_0) {
            return wait_result == WAIT_TIMEOUT ? static_cast<int32_t>(ERROR_TIMEOUT)
                                               : static_cast<int32_t>(GetLastError());
        }

        CloseHandle(thread_);
        thread_ = nullptr;
        return 0;
    }

private:
    static int32_t copy_presentation(
        const llavon_candidate_ui_presentation* source,
        Presentation& destination) {
        constexpr std::size_t legacy_presentation_size =
            offsetof(llavon_candidate_ui_presentation, anchor_top);
        constexpr std::size_t anchor_top_end =
            offsetof(llavon_candidate_ui_presentation, anchor_top) + sizeof(int32_t);
        if (!source || source->struct_size < legacy_presentation_size ||
            source->candidate_count == 0 || source->candidate_count > maximum_candidate_count ||
            !source->candidates || source->selection_index >= source->candidate_count ||
            source->layout_columns == 0 || source->layout_columns > maximum_layout_columns ||
            source->number_column >= source->layout_columns) {
            return static_cast<int32_t>(ERROR_INVALID_PARAMETER);
        }

        try {
            destination.owner_window = reinterpret_cast<HWND>(
                static_cast<ULONG_PTR>(source->owner_window));
            destination.anchor_x = source->anchor_x;
            destination.anchor_y = source->anchor_y;
            destination.anchor_top =
                source->struct_size >= anchor_top_end ? std::min(source->anchor_top, source->anchor_y)
                                                      : source->anchor_y;
            destination.selection_index = source->selection_index;
            destination.layout_columns = source->layout_columns;
            destination.number_column = source->number_column;
            destination.can_prev_page = source->can_prev_page != 0;
            destination.can_next_page = source->can_next_page != 0;
            destination.candidates.reserve(source->candidate_count);
            for (uint32_t index = 0; index < source->candidate_count; ++index) {
                const auto& value = source->candidates[index];
                if ((!value.data && value.length != 0) || value.length > maximum_candidate_length) {
                    return static_cast<int32_t>(ERROR_INVALID_PARAMETER);
                }
                destination.candidates.emplace_back(value.data ? value.data : L"", value.length);
            }
        } catch (...) {
            return static_cast<int32_t>(ERROR_NOT_ENOUGH_MEMORY);
        }
        return 0;
    }

    static DWORD WINAPI thread_entry(void* context) noexcept {
        return static_cast<Runtime*>(context)->thread_main();
    }

    DWORD thread_main() noexcept {
        bool apartment_initialized = false;
        DWORD exit_code = ERROR_GEN_FAILURE;
        try {
            winrt::init_apartment(winrt::apartment_type::single_threaded);
            apartment_initialized = true;

            const HINSTANCE instance = reinterpret_cast<HINSTANCE>(&__ImageBase);
            WNDCLASSEXW window_class{sizeof(window_class)};
            window_class.lpfnWndProc = command_window_proc;
            window_class.hInstance = instance;
            window_class.lpszClassName = command_window_class;
            if (!RegisterClassExW(&window_class) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
                throw winrt::hresult_error(HRESULT_FROM_WIN32(GetLastError()));
            }

            CandidateWindow candidate_window;
            candidate_window_ = &candidate_window;
            const HWND command_window = CreateWindowExW(
                0, command_window_class, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance, this);
            if (!command_window) {
                throw winrt::hresult_error(HRESULT_FROM_WIN32(GetLastError()));
            }

            command_window_.store(command_window, std::memory_order_release);
            start_result_.store(0, std::memory_order_relaxed);
            SetEvent(ready_event_);

            MSG message{};
            while (GetMessageW(&message, nullptr, 0, 0) > 0) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }

            candidate_window.destroy();
            candidate_window_ = nullptr;
            command_window_.store(nullptr, std::memory_order_release);
            exit_code = 0;
        } catch (const winrt::hresult_error& error) {
            DebugSink::instance().send(L"ERROR", L"runtime failed: " + std::wstring(error.message()));
            start_result_.store(static_cast<int32_t>(error.code().value), std::memory_order_relaxed);
            if (ready_event_) {
                SetEvent(ready_event_);
            }
        } catch (...) {
            DebugSink::instance().send(L"ERROR", L"runtime failed with an unknown error");
            start_result_.store(ERROR_GEN_FAILURE, std::memory_order_relaxed);
            if (ready_event_) {
                SetEvent(ready_event_);
            }
        }

        candidate_window_ = nullptr;
        command_window_.store(nullptr, std::memory_order_release);
        if (apartment_initialized) {
            winrt::uninit_apartment();
        }
        return exit_code;
    }

    static LRESULT CALLBACK command_window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        Runtime* self = nullptr;
        if (message == WM_NCCREATE) {
            const auto create = reinterpret_cast<CREATESTRUCTW*>(lparam);
            self = static_cast<Runtime*>(create->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        } else {
            self = reinterpret_cast<Runtime*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        }

        if (!self) {
            return DefWindowProcW(window, message, wparam, lparam);
        }
        if (message == present_message) {
            self->present_on_thread();
            return 0;
        }
        if (message == hide_message) {
            if (self->candidate_window_) {
                self->candidate_window_->hide();
            }
            return 0;
        }
        if (message == stop_message) {
            if (self->candidate_window_) {
                self->candidate_window_->destroy();
            }
            DestroyWindow(window);
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(window, message, wparam, lparam);
    }

    void present_on_thread() {
        std::optional<Presentation> presentation;
        {
            std::lock_guard lock(presentation_mutex_);
            presentation.swap(pending_presentation_);
        }
        if (!presentation || !candidate_window_) {
            return;
        }

        candidate_window_->set_owner_window(presentation->owner_window);
        candidate_window_->set_layout_columns(presentation->layout_columns);
        candidate_window_->set_number_column(presentation->number_column);
        candidate_window_->set_page_navigation(
            presentation->can_prev_page, presentation->can_next_page);
        candidate_window_->update_candidates(presentation->candidates);
        candidate_window_->set_selection(presentation->selection_index);
        candidate_window_->show_at(
            presentation->anchor_x, presentation->anchor_top, presentation->anchor_y);
    }

    bool post(UINT message) const noexcept {
        const HWND command_window = command_window_.load(std::memory_order_acquire);
        return command_window && PostMessageW(command_window, message, 0, 0) != FALSE;
    }

    std::mutex lifecycle_mutex_;
    std::mutex presentation_mutex_;
    std::optional<Presentation> pending_presentation_;
    HANDLE thread_ = nullptr;
    HANDLE ready_event_ = nullptr;
    std::atomic<HWND> command_window_{nullptr};
    std::atomic<int32_t> start_result_{ERROR_GEN_FAILURE};
    CandidateWindow* candidate_window_ = nullptr;
};

Runtime& runtime() {
    static Runtime instance;
    return instance;
}

}  // namespace
}  // namespace llavon::candidate

extern "C" int32_t llavon_candidate_ui_start(void) {
    return llavon::candidate::runtime().start();
}

extern "C" int32_t llavon_candidate_ui_present(
    const llavon_candidate_ui_presentation* presentation) {
    return llavon::candidate::runtime().present(presentation);
}

extern "C" void llavon_candidate_ui_hide(void) {
    llavon::candidate::runtime().hide();
}

extern "C" int32_t llavon_candidate_ui_stop(void) {
    return llavon::candidate::runtime().stop();
}
