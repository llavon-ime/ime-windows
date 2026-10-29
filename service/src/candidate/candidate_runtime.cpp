#include "candidate_ui.hpp"

#include "candidate_window.hpp"
#include "../service/debug/core_logger_adapter.hpp"
#include "../ui/ui_thread.hpp"

#include <windows.h>

#include <atomic>
#include <algorithm>
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

} // namespace

struct CandidateUi::State {
    explicit State(ui::UiThread& thread) : thread_(thread) {}

    bool present(Presentation presentation) {
        if (presentation.candidates.empty() ||
            presentation.candidates.size() > maximum_candidate_count ||
            presentation.selection_index >= presentation.candidates.size() ||
            presentation.layout_columns == 0 ||
            presentation.layout_columns > maximum_layout_columns ||
            presentation.number_column >= presentation.layout_columns ||
            std::ranges::any_of(presentation.candidates,
                [](const auto& value) { return value.size() > maximum_candidate_length; }))
            return false;
        presentation.anchor_top = std::min(presentation.anchor_top, presentation.anchor_y);

        {
            std::lock_guard lock(presentation_mutex_);
            pending_presentation_ = std::move(presentation);
        }
        return post(present_message);
    }

    void hide() noexcept {
        {
            std::lock_guard lock(presentation_mutex_);
            pending_presentation_.reset();
        }
        post(hide_message);
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
        if (!candidate_window_->initialize()) winrt::throw_hresult(E_FAIL);
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
    }
    static LRESULT CALLBACK command_window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        State* self = nullptr;
        if (message == WM_NCCREATE) {
            const auto create = reinterpret_cast<CREATESTRUCTW*>(lparam);
            self = static_cast<State*>(create->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        } else {
            self = reinterpret_cast<State*>(GetWindowLongPtrW(window, GWLP_USERDATA));
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

    ui::UiThread& thread_;
    std::mutex presentation_mutex_;
    std::optional<Presentation> pending_presentation_;
    std::atomic<HWND> command_window_{nullptr};
    std::unique_ptr<llavon::ime::core::Logger> logger_;
    std::unique_ptr<CandidateWindow> candidate_window_;
    winrt::Microsoft::UI::Xaml::ResourceDictionary theme_{nullptr};
};

CandidateUi::CandidateUi(ui::UiThread& thread) : state_(std::make_unique<State>(thread)) {
    try {
        thread.invoke([this] { state_->initialize_on_thread(); });
    } catch (...) {
        thread.invoke([this] { state_->destroy_on_thread(); });
        throw;
    }
}

CandidateUi::~CandidateUi() {
    state_->thread_.invoke([this] { state_->destroy_on_thread(); });
}

bool CandidateUi::present(Presentation presentation) {
    return state_->present(std::move(presentation));
}

void CandidateUi::hide() noexcept {
    state_->hide();
}

} // namespace llavon::candidate
