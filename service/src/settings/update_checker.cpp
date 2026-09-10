#include "update_checker.hpp"

#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Web.Http.Headers.h>
#include <winrt/Windows.Web.Http.h>
#include <winrt/base.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#define LLAVON_WIDEN_DETAIL(value) L##value
#define LLAVON_WIDEN(value) LLAVON_WIDEN_DETAIL(value)

namespace llavon::settings {
namespace {

using winrt::Windows::Data::Json::JsonObject;
using winrt::Windows::Foundation::AsyncStatus;
using winrt::Windows::Foundation::Uri;
using winrt::Windows::Web::Http::HttpClient;

constexpr std::uint64_t current_build = LLAVON_IME_BUILD_NUMBER;
constexpr wchar_t current_commit[] = LLAVON_WIDEN(LLAVON_IME_BUILD_COMMIT);
constexpr wchar_t latest_manifest_url[] =
    L"https://github.com/llavon-ime/ime-windows/releases/download/latest/latest.json";
constexpr wchar_t latest_release_url[] =
    L"https://github.com/llavon-ime/ime-windows/releases/tag/latest";
constexpr auto update_timeout = std::chrono::seconds(2);
constexpr double largest_exact_json_integer = 9007199254740991.0;

class WinrtApartment final {
public:
    WinrtApartment() {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
    }

    WinrtApartment(const WinrtApartment&) = delete;
    WinrtApartment& operator=(const WinrtApartment&) = delete;

    ~WinrtApartment() {
        winrt::uninit_apartment();
    }
};

bool is_commit_id(std::wstring_view value) noexcept {
    if (value.size() < 7 || value.size() > 64) {
        return false;
    }
    for (const wchar_t character : value) {
        const bool digit = character >= L'0' && character <= L'9';
        const bool lower = character >= L'a' && character <= L'f';
        const bool upper = character >= L'A' && character <= L'F';
        if (!digit && !lower && !upper) {
            return false;
        }
    }
    return true;
}

std::uint64_t json_unsigned(const JsonObject& json, const wchar_t* name) {
    const double value = json.GetNamedNumber(name);
    if (!std::isfinite(value) || value < 0 || value > largest_exact_json_integer ||
        std::floor(value) != value) {
        throw std::runtime_error("latest.json contains an invalid integer");
    }
    return static_cast<std::uint64_t>(value);
}

UpdateCheckResult base_result() {
    UpdateCheckResult result;
    result.current_build = current_build;
    result.current_commit = current_commit;
    return result;
}

UpdateCheckResult perform_check() {
    UpdateCheckResult result = base_result();
    if (!is_commit_id(current_commit)) {
        throw std::runtime_error("this build does not contain a repository commit id");
    }

    WinrtApartment apartment;
    HttpClient client;
    const auto headers = client.DefaultRequestHeaders();
    headers.UserAgent().ParseAdd(L"LlavonIME update-checker");
    headers.TryAppendWithoutValidation(L"Cache-Control", L"no-cache, no-store");
    headers.TryAppendWithoutValidation(L"Pragma", L"no-cache");

    const auto operation = client.GetStringAsync(Uri(latest_manifest_url));
    if (operation.wait_for(update_timeout) == AsyncStatus::Started) {
        operation.Cancel();
        throw std::runtime_error("update check timed out after 2 seconds");
    }
    const winrt::hstring body = operation.GetResults();

    JsonObject manifest{nullptr};
    if (!JsonObject::TryParse(body, manifest) || !manifest.HasKey(L"schema") ||
        !manifest.HasKey(L"build") || !manifest.HasKey(L"commit")) {
        throw std::runtime_error("latest.json is missing required fields");
    }
    if (json_unsigned(manifest, L"schema") != 1) {
        throw std::runtime_error("latest.json uses an unsupported schema");
    }

    result.latest_build = json_unsigned(manifest, L"build");
    const winrt::hstring remote_commit = manifest.GetNamedString(L"commit");
    result.latest_commit.assign(remote_commit.c_str(), remote_commit.size());
    result.release_url = latest_release_url;
    if (!is_commit_id(result.latest_commit) || result.latest_build == 0) {
        throw std::runtime_error("latest.json contains invalid build identity");
    }

    if (result.current_build == 0) {
        result.status = result.current_commit == result.latest_commit
                            ? UpdateCheckStatus::up_to_date
                            : UpdateCheckStatus::development_build;
    } else if (result.current_build < result.latest_build) {
        result.status = UpdateCheckStatus::update_available;
    } else if (result.current_build > result.latest_build) {
        result.status = UpdateCheckStatus::local_newer;
    } else if (result.current_commit == result.latest_commit) {
        result.status = UpdateCheckStatus::up_to_date;
    } else {
        throw std::runtime_error("build number matches latest but commit id differs");
    }
    return result;
}

std::wstring widen_error(const std::exception& error) {
    const std::string_view message(error.what());
    return std::wstring(message.begin(), message.end());
}

}  // namespace

UpdateChecker::~UpdateChecker() {
    if (worker_.joinable()) {
        worker_.join();
    }
}

std::uint64_t UpdateChecker::installed_build_number() noexcept {
    return current_build;
}

std::wstring_view UpdateChecker::installed_commit() noexcept {
    return current_commit;
}

bool UpdateChecker::check_async(Completion completion) {
    if (!completion || checking_.exchange(true, std::memory_order_acq_rel)) {
        return false;
    }
    if (worker_.joinable()) {
        worker_.join();
    }

    worker_ = std::thread([this, completion = std::move(completion)]() mutable {
        UpdateCheckResult result = check_now();
        try {
            completion(std::move(result));
        } catch (...) {
        }
        checking_.store(false, std::memory_order_release);
    });
    return true;
}

UpdateCheckResult UpdateChecker::check_now() noexcept {
    try {
        return perform_check();
    } catch (const winrt::hresult_error& error) {
        UpdateCheckResult result = base_result();
        result.error_message = error.message();
        return result;
    } catch (const std::exception& error) {
        UpdateCheckResult result = base_result();
        result.error_message = widen_error(error);
        return result;
    } catch (...) {
        UpdateCheckResult result = base_result();
        result.error_message = L"unknown update-check error";
        return result;
    }
}

}  // namespace llavon::settings

#undef LLAVON_WIDEN
#undef LLAVON_WIDEN_DETAIL
