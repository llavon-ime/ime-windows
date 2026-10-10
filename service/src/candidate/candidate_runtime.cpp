#include "candidate_ui_api.h"

#include "candidate_window.hpp"
#include "../service/debug/core_logger_adapter.hpp"
#include "../ui/ui_thread.hpp"

#include <windows.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <memory>
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
        if (connected_)
            return 0;
        const auto result = llavon_ui_thread_acquire();
        if (result != 0)
            return result;
        connected_ = true;
        const auto initialized = llavon_ui_thread_invoke(
            [](void* context) { static_cast<Runtime*>(context)->initialize_on_thread(); }, this);
        if (initialized != 0) {
            llavon_ui_thread_invoke(
                [](void* context) { static_cast<Runtime*>(context)->destroy_on_thread(); }, this);
            if (llavon_ui_thread_release() == 0)
                connected_ = false;
        }
        return initialized;
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
        if (!connected_)
            return 0;
        {
            std::lock_guard presentation_lock(presentation_mutex_);
            pending_presentation_.reset();
        }
        const auto destroyed = llavon_ui_thread_invoke(
            [](void* context) { static_cast<Runtime*>(context)->destroy_on_thread(); }, this);
        if (destroyed != 0)
            return destroyed;
        const auto result = llavon_ui_thread_release();
        if (result == 0)
            connected_ = false;
        return result;
    }

private:
    static int32_t copy_presentation(const llavon_candidate_ui_presentation* source,
                                     Presentation& destination) {
        constexpr std::size_t legacy_presentation_size =
            offsetof(llavon_candidate_ui_presentation, anchor_top);
        constexpr std::size_t anchor_top_end =
            offsetof(llavon_candidate_ui_presentation, anchor_top) + sizeof(int32_t);
        if (!source || source->struct_size < legacy_presentation_size || source->candidate_count == 0 ||
            source->candidate_count > maximum_candidate_count || !source->candidates ||
            source->selection_index >= source->candidate_count || source->layout_columns == 0 ||
            source->layout_columns > maximum_layout_columns ||
            source->number_column >= source->layout_columns) {
            return static_cast<int32_t>(ERROR_INVALID_PARAMETER);
        }

        try {
            destination.owner_window = reinterpret_cast<HWND>(static_cast<ULONG_PTR>(source->owner_window));
            destination.anchor_x = source->anchor_x;
            destination.anchor_y = source->anchor_y;
            destination.anchor_top = source->struct_size >= anchor_top_end
                                         ? std::min(source->anchor_top, source->anchor_y)
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

    void initialize_on_thread() {
        logger_ = std::make_unique<llavon::service::debug::CoreLoggerAdapter>("candidate-ui");
        using namespace winrt::Microsoft::UI::Xaml;
        theme_ = ui::load_xaml_resource(IDR_CANDIDATE_THEME_XAML).as<ResourceDictionary>();
        Application::Current().Resources().MergedDictionaries().Append(theme_);
        const HINSTANCE instance = reinterpret_cast<HINSTANCE>(&__ImageBase);
        WNDCLASSEXW window_class{sizeof(window_class)};
        window_class.lpfnWndProc = command_window_proc;
        window_class.hInstance = instance;
        window_class.lpszClassName = command_window_class;
        if (!RegisterClassExW(&window_class) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            winrt::throw_last_error();
        }
        candidate_window_ = std::make_unique<CandidateWindow>(*logger_);
        const HWND command_window = CreateWindowExW(0, command_window_class, L"", 0, 0, 0, 0, 0, HWND_MESSAGE,
                                                    nullptr, instance, this);
        if (!command_window)
            winrt::throw_last_error();
        command_window_.store(command_window, std::memory_order_release);
    }

    void destroy_on_thread() {
        const HWND command = command_window_.exchange(nullptr, std::memory_order_acq_rel);
        if (command) {
            DestroyWindow(command);
            MSG message{};
            while (PeekMessageW(&message, command, 0, 0, PM_REMOVE)) {
            }
        }
        candidate_window_.reset();
        logger_.reset();
        if (theme_) {
            const auto dictionaries =
                winrt::Microsoft::UI::Xaml::Application::Current().Resources().MergedDictionaries();
            uint32_t index = 0;
            if (dictionaries.IndexOf(theme_, index))
                dictionaries.RemoveAt(index);
            theme_ = nullptr;
        }
        UnregisterClassW(command_window_class, reinterpret_cast<HINSTANCE>(&__ImageBase));
        UnregisterClassW(L"TSF_CandidatePopupWindow", reinterpret_cast<HINSTANCE>(&__ImageBase));
        winrt::clear_factory_cache();
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
            try {
                self->present_on_thread();
            } catch (const winrt::hresult_error& error) {
                self->logger_->log(LogInformation::debug, [message = error.message()] {
                    return std::format("[ERROR] Presentation failed: {}", winrt::to_string(message));
                });
                if (self->candidate_window_)
                    self->candidate_window_->hide();
            } catch (...) {
                self->logger_->log(LogInformation::debug,
                                   "[ERROR] Presentation failed with an unknown error");
                if (self->candidate_window_)
                    self->candidate_window_->hide();
            }
            return 0;
        }
        if (message == hide_message) {
            if (self->candidate_window_) {
                self->candidate_window_->hide();
            }
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
        candidate_window_->set_page_navigation(presentation->can_prev_page, presentation->can_next_page);
        candidate_window_->update_candidates(presentation->candidates);
        candidate_window_->set_selection(presentation->selection_index);
        candidate_window_->show_at(presentation->anchor_x, presentation->anchor_top, presentation->anchor_y);
    }

    bool post(UINT message) const noexcept {
        const HWND command_window = command_window_.load(std::memory_order_acquire);
        return command_window && PostMessageW(command_window, message, 0, 0) != FALSE;
    }

    std::mutex lifecycle_mutex_;
    std::mutex presentation_mutex_;
    std::optional<Presentation> pending_presentation_;
    bool connected_ = false;
    std::atomic<HWND> command_window_{nullptr};
    std::unique_ptr<llavon::ime::core::Logger> logger_;
    std::unique_ptr<CandidateWindow> candidate_window_;
    winrt::Microsoft::UI::Xaml::ResourceDictionary theme_{nullptr};
};

Runtime& runtime() {
    static Runtime instance;
    return instance;
}

} // namespace
} // namespace llavon::candidate

extern "C" int32_t llavon_candidate_ui_start(void) {
    return llavon::candidate::runtime().start();
}

extern "C" int32_t llavon_candidate_ui_present(const llavon_candidate_ui_presentation* presentation) {
    return llavon::candidate::runtime().present(presentation);
}

extern "C" void llavon_candidate_ui_hide(void) {
    llavon::candidate::runtime().hide();
}

extern "C" int32_t llavon_candidate_ui_stop(void) {
    return llavon::candidate::runtime().stop();
}
