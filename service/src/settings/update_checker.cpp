#include "update_checker.hpp"

#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Web.Http.Headers.h>
#include <winrt/Windows.Web.Http.h>
#include <winrt/base.h>

#include <chrono>
#include <cmath>
#include <compare>
#include <cstdint>
#include <limits>
#include <optional>
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
constexpr wchar_t current_version[] = LLAVON_WIDEN(LLAVON_IME_VERSION);
constexpr wchar_t latest_manifest_url[] =
    L"https://github.com/llavon-ime/ime-windows/releases/download/latest/latest.json";
constexpr wchar_t latest_release_url[] =
    L"https://github.com/llavon-ime/ime-windows/releases/tag/latest";
constexpr auto update_timeout = std::chrono::seconds(2);
constexpr double largest_exact_json_integer = 9007199254740991.0;

struct CalVersion {
    std::uint64_t year;
    std::uint64_t month;
    std::uint64_t day;
    std::uint64_t revision;

    auto operator<=>(const CalVersion&) const = default;
};

std::optional<std::uint64_t> parse_calver_component(std::wstring_view value) noexcept {
    if (value.empty()) {
        return std::nullopt;
    }
    std::uint64_t result = 0;
    for (const wchar_t character : value) {
        if (character < L'0' || character > L'9') {
            return std::nullopt;
        }
        const auto digit = static_cast<std::uint64_t>(character - L'0');
        if (result > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) {
            return std::nullopt;
        }
        result = result * 10 + digit;
    }
    return result;
}

std::optional<CalVersion> parse_calver(std::wstring_view value) noexcept {
    std::wstring_view components[4];
    for (std::size_t index = 0; index < 3; ++index) {
        const auto separator = value.find(L'.');
        if (separator == std::wstring_view::npos) {
            return std::nullopt;
        }
        components[index] = value.substr(0, separator);
        value.remove_prefix(separator + 1);
    }
    if (value.find(L'.') != std::wstring_view::npos) {
        return std::nullopt;
    }
    components[3] = value;

    if (components[0].size() != 4 || components[1].size() != 2 ||
        components[2].size() != 2) {
        return std::nullopt;
    }
    const auto year = parse_calver_component(components[0]);
    const auto month = parse_calver_component(components[1]);
    const auto day = parse_calver_component(components[2]);
    const auto revision = parse_calver_component(components[3]);
    if (!year || !month || !day || !revision || *year < 2000 || *year > 9999) {
        return std::nullopt;
    }

    const std::chrono::year_month_day date{
        std::chrono::year{static_cast<int>(*year)},
        std::chrono::month{static_cast<unsigned>(*month)},
        std::chrono::day{static_cast<unsigned>(*day)}};
    if (!date.ok()) {
        return std::nullopt;
    }
    return CalVersion{*year, *month, *day, *revision};
}

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
    result.current_version = current_version;
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
        !manifest.HasKey(L"build") || !manifest.HasKey(L"commit") ||
        !manifest.HasKey(L"version")) {
        throw std::runtime_error("latest.json is missing required fields");
    }
    if (json_unsigned(manifest, L"schema") != 1) {
        throw std::runtime_error("latest.json uses an unsupported schema");
    }

    result.latest_build = json_unsigned(manifest, L"build");
    const winrt::hstring remote_commit = manifest.GetNamedString(L"commit");
    result.latest_commit.assign(remote_commit.c_str(), remote_commit.size());
    const winrt::hstring remote_version = manifest.GetNamedString(L"version");
    result.latest_version.assign(remote_version.c_str(), remote_version.size());
    result.release_url = latest_release_url;
    const auto parsed_latest_version = parse_calver(result.latest_version);
    if (!is_commit_id(result.latest_commit) || result.latest_build == 0 ||
        !parsed_latest_version) {
        throw std::runtime_error("latest.json contains invalid build identity");
    }

    const auto parsed_current_version = parse_calver(result.current_version);
    if (result.current_build == 0 || !parsed_current_version) {
        result.status = result.current_commit == result.latest_commit
                            ? UpdateCheckStatus::up_to_date
                            : UpdateCheckStatus::development_build;
    } else if (*parsed_current_version < *parsed_latest_version) {
        result.status = UpdateCheckStatus::update_available;
    } else if (*parsed_current_version > *parsed_latest_version) {
        result.status = UpdateCheckStatus::local_newer;
    } else if (result.current_commit == result.latest_commit) {
        result.status = UpdateCheckStatus::up_to_date;
    } else {
        throw std::runtime_error("CalVer matches latest but commit id differs");
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

std::wstring_view UpdateChecker::installed_version() noexcept {
    return current_version;
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
