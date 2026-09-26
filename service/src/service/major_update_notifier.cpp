#include "major_update_notifier.hpp"

#include "../settings/update_checker.hpp"

#include <windows.h>
#include <winrt/Windows.Data.Xml.Dom.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.Notifications.h>
#include <winrt/base.h>

#include <chrono>
#include <utility>

namespace llavon::service {
namespace {

constexpr wchar_t app_user_model_id[] = L"Llavon.IME.Windows";
constexpr auto update_delay = std::chrono::minutes(10);

struct WinrtApartment {
    WinrtApartment() { winrt::init_apartment(winrt::apartment_type::multi_threaded); }
    ~WinrtApartment() { winrt::uninit_apartment(); }
};

}  // namespace

struct MajorUpdateNotifier::ActivationState {
    explicit ActivationState(std::function<void()> callback)
        : open_settings(std::move(callback)) {}

    std::mutex mutex;
    std::function<void()> open_settings;
};

MajorUpdateNotifier::MajorUpdateNotifier(std::function<void()> open_settings)
    : activation_(std::make_shared<ActivationState>(std::move(open_settings))),
      worker_([this](std::stop_token stop) { run(stop); }) {}

MajorUpdateNotifier::~MajorUpdateNotifier() {
    {
        std::lock_guard lock(activation_->mutex);
        activation_->open_settings = {};
    }
    worker_.request_stop();
}

void MajorUpdateNotifier::run(std::stop_token stop) noexcept {
    try {
        {
            std::unique_lock lock(wait_mutex_);
            wake_.wait_for(lock, stop, update_delay, [] { return false; });
        }
        if (stop.stop_requested() || settings::UpdateChecker::installed_build_number() == 0) {
            return;
        }

        const auto result = settings::UpdateChecker::check_now();
        if (stop.stop_requested() ||
            result.status != settings::UpdateCheckStatus::update_available ||
            result.major_update_build == 0 ||
            result.current_build >= result.major_update_build) {
            return;
        }

        WinrtApartment apartment;
        using namespace winrt::Windows::UI::Notifications;
        auto xml = ToastNotificationManager::GetTemplateContent(ToastTemplateType::ToastText02);
        const auto text = xml.GetElementsByTagName(L"text");
        text.Item(0).AppendChild(xml.CreateTextNode(L"Llavon 輸入法重大更新"));
        text.Item(1).AppendChild(xml.CreateTextNode(result.major_update_message));
        const auto audio = xml.CreateElement(L"audio");
        audio.SetAttribute(L"silent", L"true");
        xml.DocumentElement().AppendChild(audio);

        ToastNotification toast(xml);
        const std::weak_ptr target(activation_);
        toast.Activated([target](const auto&, const auto&) {
            if (const auto state = target.lock()) {
                std::lock_guard lock(state->mutex);
                if (state->open_settings) state->open_settings();
            }
        });
        if (stop.stop_requested()) return;
        ToastNotificationManager::CreateToastNotifier(app_user_model_id).Show(toast);

        // Keep the toast and its click handler alive until service shutdown.
        std::unique_lock lock(wait_mutex_);
        wake_.wait(lock, stop, [] { return false; });
    } catch (const winrt::hresult_error& error) {
        OutputDebugStringW(L"[service] update toast failed: ");
        OutputDebugStringW(error.message().c_str());
        OutputDebugStringW(L"\n");
    } catch (...) {
        OutputDebugStringW(L"[service] major update notification failed\n");
    }
}

}  // namespace llavon::service
