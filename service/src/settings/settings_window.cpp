#include "settings_window.hpp"

#include "../resource.h"
#include "settings_resources.h"
#include "xaml_resource.hpp"

#include <dwmapi.h>
#include <shobjidl_core.h>
#include <winrt/Microsoft.UI.Interop.h>
#include <rfl/json.hpp>
#include <utf8/cpp20.h>

#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <format>
#include <iomanip>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.Text.h>
#include <winrt/Windows.UI.ViewManagement.h>
#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>
#include <winrt/Microsoft.UI.Xaml.Data.h>
#include <winrt/Microsoft.UI.Xaml.Input.h>
#include <winrt/Microsoft.UI.Xaml.Markup.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Windows.UI.h>

namespace llavon::settings {

extern "C" IMAGE_DOS_HEADER __ImageBase;

struct SettingsWindow::UpdateNotificationTarget {
    std::mutex mutex;
    HWND window = nullptr;
};

namespace {

using namespace winrt::Windows::UI::Text;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;
using namespace winrt::Microsoft::UI::Xaml::Controls::Primitives;
using namespace winrt::Microsoft::UI::Xaml::Media;

constexpr wchar_t window_class_name[] = L"LlavonImeSettingsWindow";
constexpr UINT update_result_message = WM_APP + 10;
constexpr double body_text_size = 14;
constexpr double caption_text_size = 12;
constexpr std::size_t training_page_size = 100;

std::wstring training_count_label(std::size_t count) {
    return L"共 " + std::to_wstring(count) + L" 筆";
}

SolidColorBrush solid_brush(std::uint8_t red, std::uint8_t green, std::uint8_t blue) {
    return SolidColorBrush(winrt::Windows::UI::Color{255, red, green, blue});
}

TextBlock make_text(const wchar_t* value, double size, FontWeight weight = FontWeights::Normal()) {
    TextBlock block;
    block.Text(value);
    block.FontFamily(FontFamily(L"Microsoft JhengHei UI"));
    block.FontSize(size);
    block.FontWeight(weight);
    block.TextWrapping(TextWrapping::Wrap);
    return block;
}

std::wstring utc8_timestamp(std::u16string_view value) {
    const std::string source = utf8::utf16to8(std::u16string(value));
    std::chrono::sys_time<std::chrono::milliseconds> utc;
    std::istringstream input(source);
    std::chrono::from_stream(input, "%FT%TZ", utc);
    if (input.fail()) {
        std::wstring fallback;
        fallback.reserve(value.size());
        std::ranges::transform(value, std::back_inserter(fallback),
            [](char16_t character) { return static_cast<wchar_t>(character); });
        return fallback;
    }
    return std::format(L"{:%Y/%m/%d %H:%M}",
        std::chrono::floor<std::chrono::minutes>(utc + std::chrono::hours{8}));
}

template <typename T>
T named(const FrameworkElement& root, const wchar_t* name) {
    const auto element = root.FindName(name);
    if (!element) winrt::throw_hresult(E_INVALIDARG);
    return element.as<T>();
}

bool system_uses_dark_theme() {
    const auto foreground = winrt::Windows::UI::ViewManagement::UISettings().GetColorValue(
        winrt::Windows::UI::ViewManagement::UIColorType::Foreground);
    return 5u * foreground.G + 2u * foreground.R + foreground.B > 8u * 128u;
}

std::wstring short_commit(std::wstring_view commit) {
    if (commit == L"unknown" || commit.size() < 7) {
        return L"開發版本";
    }
    return std::wstring(commit.substr(0, 7));
}

std::wstring build_identity(
    std::wstring_view version, std::uint64_t build, std::wstring_view commit) {
    if (build == 0) {
        return std::wstring(version) + L"（" + short_commit(commit) + L"）";
    }
    return std::wstring(version) + L"（建置 #" + std::to_wstring(build) + L"、" +
           short_commit(commit) + L"）";
}

const char16_t* backend_label(std::int32_t backend) {
    switch (backend) {
        case LLAVON_SETTINGS_BACKEND_CUDA:
            return u"CUDA";
        case LLAVON_SETTINGS_BACKEND_VULKAN:
            return u"Vulkan";
        case LLAVON_SETTINGS_BACKEND_CPU:
            return u"CPU";
        default:
            return u"自動";
    }
}

std::u16string device_label(const InferenceDeviceOption& device) {
    std::u16string label = !device.description.empty() ? device.description : device.name;
    if (label.empty()) label = device.device_id;
    label += u"（";
    label += backend_label(device.backend);
    if (device.backend != LLAVON_SETTINGS_BACKEND_CPU && device.memory_total != 0) {
        label += u"，";
        std::ostringstream memory;
        memory << std::fixed << std::setprecision(1)
               << static_cast<double>(device.memory_total) / 1024.0 / 1024.0 / 1024.0
               << " GiB";
        label += utf8::utf8to16(memory.str());
    }
    label += u"）";
    return label;
}

std::u16string trim(std::u16string value) {
    const auto is_space = [](char16_t character) {
        return std::iswspace(static_cast<wchar_t>(character)) != 0;
    };
    const auto first = std::find_if_not(value.begin(), value.end(), is_space);
    const auto last = std::find_if_not(value.rbegin(), value.rend(), is_space).base();
    if (first >= last) return {};
    return std::u16string(first, last);
}

std::u16string to_utf16(const winrt::hstring& value) {
    return std::u16string(value.begin(), value.end());
}

winrt::hstring to_hstring(std::u16string_view value) {
    if (value.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw std::length_error("text is too long for Windows Runtime");
    }
    if (value.empty()) return {};

    std::vector<wchar_t> buffer(value.size());
    std::ranges::transform(value, buffer.begin(), [](char16_t code_unit) {
        return static_cast<wchar_t>(code_unit);
    });
    return winrt::hstring(buffer.data(), static_cast<std::uint32_t>(buffer.size()));
}

struct DisplayCharacter {
    char32_t value = 0;
    std::u16string text;
};

std::optional<std::vector<DisplayCharacter>> split_characters(
    const std::u16string& text) {
    std::vector<DisplayCharacter> result;
    try {
        for (auto position = text.cbegin(); position != text.cend();) {
            const auto start = position;
            const char32_t value = utf8::next16(position, text.cend());
            result.push_back(DisplayCharacter{
                .value = value,
                .text = std::u16string(start, position),
            });
        }
    } catch (const utf8::exception&) {
        return std::nullopt;
    }
    return result;
}

std::u16string display_tail(const std::vector<DisplayCharacter>& characters,
                            std::size_t maximum) {
    const std::size_t omitted = characters.size() > maximum
        ? characters.size() - maximum : 0;
    std::u16string result = omitted ? u"…" : u"";
    for (std::size_t index = omitted; index < characters.size(); ++index) {
        result += characters[index].text;
    }
    return result;
}

std::vector<std::u16string> split_reading_tokens(std::u16string_view reading) {
    std::vector<std::u16string> result;
    for (std::size_t position = 0; position < reading.size();) {
        while (position < reading.size() &&
               std::iswspace(static_cast<wchar_t>(reading[position]))) {
            ++position;
        }
        const std::size_t start = position;
        while (position < reading.size() &&
               !std::iswspace(static_cast<wchar_t>(reading[position]))) {
            ++position;
        }
        if (position != start) result.emplace_back(reading.substr(start, position - start));
    }
    return result;
}

void render_training_item(const TrainingDataOption& item, const Grid& root,
                          const FrameworkElement& owner, bool dark) {
    constexpr std::size_t maximum_context_characters = 14;
    constexpr std::size_t maximum_answer_characters = 14;
    const auto characters = split_characters(item.answer);
    const auto context_characters = split_characters(item.context);
    const auto context = named<TextBlock>(root, L"ContextText");
    context.Text(to_hstring(context_characters
        ? display_tail(*context_characters, maximum_context_characters)
        : item.context));
    context.Visibility(item.context.empty() ? Visibility::Collapsed : Visibility::Visible);

    const bool inline_context = context_characters && characters &&
        !context_characters->empty() && context_characters->size() <= 6 &&
        characters->size() <= 7;
    const bool truncated =
        (context_characters && context_characters->size() > maximum_context_characters) ||
        (characters && characters->size() > maximum_answer_characters);
    if (truncated) {
        const auto tooltip = load_xaml_resource(IDR_LORA_ITEM_TOOLTIP_XAML).as<ToolTip>();
        const auto tooltip_root = tooltip.as<FrameworkElement>();
        const auto full_context = named<TextBlock>(tooltip_root, L"FullContext");
        full_context.Text(to_hstring(item.context));
        full_context.Visibility(item.context.empty() ? Visibility::Collapsed : Visibility::Visible);
        named<TextBlock>(tooltip_root, L"FullAnswer").Text(to_hstring(
            item.context.empty() ? item.answer : u"→ " + item.answer));
        const auto full_reading = named<TextBlock>(tooltip_root, L"FullReading");
        full_reading.Text(item.reading.empty() ? L"" : to_hstring(u"[" + item.reading + u"]"));
        full_reading.Visibility(item.reading.empty() ? Visibility::Collapsed : Visibility::Visible);
        ToolTipService::SetToolTip(owner, tooltip);
    } else {
        ToolTipService::SetToolTip(owner, nullptr);
    }
    const auto highlight = named<Border>(root, L"AnswerHighlight");
    highlight.Background(dark ? solid_brush(35, 56, 77) : solid_brush(228, 240, 252));
    highlight.BorderBrush(dark ? solid_brush(67, 106, 140) : solid_brush(193, 221, 247));
    Grid::SetRow(highlight, item.context.empty() || inline_context ? 0 : 1);
    Grid::SetColumn(highlight, inline_context ? 1 : 0);
    Grid::SetColumnSpan(highlight, inline_context ? 1 : 2);
    Grid::SetRow(named<TextBlock>(root, L"RevisedTag"),
                 item.context.empty() || inline_context ? 1 : 2);
    const auto lines = named<StackPanel>(root, L"AnswerLines");
    const auto fallback_answer = named<TextBlock>(root, L"AnswerFallback");
    lines.Children().Clear();

    const auto readings = split_reading_tokens(item.reading);
    if (characters && !characters->empty()) {
        fallback_answer.Visibility(Visibility::Collapsed);
        lines.Visibility(Visibility::Visible);
        const bool aligned = characters->size() == readings.size();
        const std::size_t omitted = characters->size() > maximum_answer_characters
            ? characters->size() - maximum_answer_characters : 0;
        const std::size_t visible_count = characters->size() - omitted + (omitted ? 1 : 0);
        const std::size_t characters_per_line = inline_context ? 7 : 9;
        StackPanel line{nullptr};
        for (std::size_t index = 0; index < visible_count; ++index) {
            if (index % characters_per_line == 0) {
                line = StackPanel();
                line.Orientation(Orientation::Horizontal);
                lines.Children().Append(line);
            }
            auto cell = load_xaml_resource(IDR_LORA_RUBY_CELL_XAML).as<Grid>();
            const bool ellipsis = omitted && index == 0;
            if (ellipsis) {
                named<TextBlock>(cell, L"Character").Text(L"…");
            } else {
                const std::size_t source_index = omitted + index - (omitted ? 1 : 0);
                const auto& character = (*characters)[source_index].text;
                named<TextBlock>(cell, L"Character").Text(to_hstring(character));
                named<TextBlock>(cell, L"Reading").Text(
                    aligned && readings[source_index] != character
                        ? to_hstring(readings[source_index]) : L"");
            }
            line.Children().Append(cell);
        }
    } else {
        lines.Visibility(Visibility::Collapsed);
        fallback_answer.Text(to_hstring(item.answer));
        fallback_answer.Visibility(Visibility::Visible);
    }
    named<TextBlock>(root, L"RevisedTag").Visibility(
        item.revice ? Visibility::Visible : Visibility::Collapsed);
}

std::filesystem::path module_directory() {
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD copied = GetModuleFileNameW(
            reinterpret_cast<HMODULE>(&__ImageBase), buffer.data(),
            static_cast<DWORD>(buffer.size()));
        if (copied == 0) return {};
        if (copied < buffer.size() - 1) {
            buffer.resize(copied);
            return std::filesystem::path(buffer).parent_path();
        }
        buffer.resize(buffer.size() * 2);
    }
}

std::filesystem::path resolve_bopomofo_table_path() {
    std::vector<std::filesystem::path> candidates;

    const DWORD environment_size = GetEnvironmentVariableW(L"LLAVON_IME_TABLES_DIR", nullptr, 0);
    if (environment_size != 0) {
        std::wstring tables_directory(environment_size, L'\0');
        const DWORD copied = GetEnvironmentVariableW(
            L"LLAVON_IME_TABLES_DIR", tables_directory.data(), environment_size);
        if (copied != 0) {
            tables_directory.resize(copied);
            candidates.emplace_back(
                std::filesystem::path(tables_directory) / L"bopomofo_char.json");
        }
    }

    const auto directory = module_directory();
    if (!directory.empty()) {
        candidates.emplace_back(directory.parent_path() / L"tables" / L"bopomofo_char.json");
        candidates.emplace_back(directory.parent_path().parent_path().parent_path() /
                                L"ime-core" / L"table" / L"bopomofo_char.json");
    }
    candidates.emplace_back(
        std::filesystem::current_path() / L"ime-core" / L"table" / L"bopomofo_char.json");

    std::error_code error;
    for (const auto& candidate : candidates) {
        if (std::filesystem::is_regular_file(candidate, error)) {
            return candidate;
        }
        error.clear();
    }
    return {};
}

class BopomofoTable final {
public:
    static const BopomofoTable& instance() {
        static const BopomofoTable table;
        return table;
    }

    const std::vector<std::u16string>& lookup(char32_t character) const {
        static const std::vector<std::u16string> empty;
        const auto found = readings_.find(character);
        return found == readings_.end() ? empty : found->second;
    }

private:
    BopomofoTable() noexcept {
        try {
            const auto path = resolve_bopomofo_table_path();
            if (path.empty()) return;

            std::ifstream input(path, std::ios::binary);
            if (!input) return;
            const std::string body{
                std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
            auto parsed = rfl::json::read<
                std::unordered_map<std::string, std::vector<std::string>>>(body);
            auto mapping = std::move(parsed).value();
            std::unordered_map<char32_t, std::vector<std::pair<std::size_t, std::u16string>>>
                candidates;
            for (const auto& [reading_utf8, characters] : mapping) {
                const std::u16string reading = utf8::utf8to16(reading_utf8);
                if (reading.empty()) continue;
                for (std::size_t rank = 0; rank < characters.size(); ++rank) {
                    const auto code_points = utf8::utf8to32(characters[rank]);
                    if (code_points.size() == 1) {
                        candidates[code_points.front()].emplace_back(rank, reading);
                    }
                }
            }

            for (auto& [character, choices] : candidates) {
                std::ranges::sort(choices, [](const auto& left, const auto& right) {
                    return left.first != right.first ? left.first < right.first
                                                     : left.second < right.second;
                });
                auto& readings = readings_[character];
                for (auto& [rank, reading] : choices) {
                    static_cast<void>(rank);
                    if (std::ranges::find(readings, reading) == readings.end()) {
                        readings.push_back(std::move(reading));
                    }
                }
            }
        } catch (...) {
            readings_.clear();
        }
    }

    std::unordered_map<char32_t, std::vector<std::u16string>> readings_;
};

}  // namespace

SettingsWindow::SettingsWindow(SettingsConfiguration configuration)
    : configuration_(std::move(configuration)) {}

SettingsWindow::~SettingsWindow() {
    destroy();
}

bool SettingsWindow::create(HINSTANCE instance) {
    if (window_) {
        return true;
    }

    WNDCLASSEXW window_class{sizeof(window_class)};
    window_class.style = CS_HREDRAW | CS_VREDRAW;
    window_class.lpfnWndProc = window_proc;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hIcon =
        LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_LLAVON_TRAY));
    if (!window_class.hIcon) {
        window_class.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    }
    window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    window_class.lpszClassName = window_class_name;
    window_class.hIconSm = window_class.hIcon;

    if (!RegisterClassExW(&window_class) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }

    window_ = CreateWindowExW(0, window_class_name, L"Llavon 輸入法設定", WS_OVERLAPPEDWINDOW,
                              CW_USEDEFAULT, CW_USEDEFAULT, 860, 760, nullptr, nullptr, instance, this);
    if (!window_) {
        return false;
    }

    update_target_ = std::make_shared<UpdateNotificationTarget>();
    update_target_->window = window_;

    try {
        initialize_xaml_island();
        build_page();
        update_theme();
    } catch (...) {
        destroy();
        throw;
    }
    return true;
}

void SettingsWindow::show() noexcept {
    if (!window_) {
        return;
    }
    ShowWindow(window_, IsIconic(window_) ? SW_RESTORE : SW_SHOWNORMAL);
    SetForegroundWindow(window_);
    try {
        if (xaml_source_ && !xaml_source_.HasFocus()) {
            xaml_source_.NavigateFocus(Hosting::XamlSourceFocusNavigationRequest(
                Hosting::XamlSourceFocusNavigationReason::First));
        }
    } catch (...) {
        OutputDebugStringW(L"[settings-ui] unable to focus WinUI island\n");
    }
    begin_update_check();
}

void SettingsWindow::set_pending_count(std::size_t count) {
    if (!pending_summary_) return;
    std::wstring text = L"尚未訓練：";
    text += std::to_wstring(count);
    text += L" 筆";
    pending_summary_.Text(text);
}

void SettingsWindow::hide() const noexcept {
    if (window_) {
        ShowWindow(window_, SW_HIDE);
    }
}

void SettingsWindow::destroy() noexcept {
    deactivate_update_target();
    discard_pending_update_results();
    hide();
    close_xaml();
    if (window_) {
        const HWND window = window_;
        DestroyWindow(window);
        if (window_ == window) {
            window_ = nullptr;
        }
    }
}

LRESULT CALLBACK SettingsWindow::window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    SettingsWindow* self = nullptr;
    if (message == WM_NCCREATE) {
        const auto create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        self = static_cast<SettingsWindow*>(create->lpCreateParams);
        self->window_ = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<SettingsWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    }
    return self ? self->handle_message(message, wparam, lparam)
                : DefWindowProcW(window, message, wparam, lparam);
}

LRESULT SettingsWindow::handle_message(UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == update_result_message) {
        std::unique_ptr<UpdateCheckResult> result(
            reinterpret_cast<UpdateCheckResult*>(lparam));
        if (result) {
            apply_update_result(std::move(*result));
        }
        return 0;
    }

    switch (message) {
        case WM_CLOSE:
            hide();
            return 0;
        case WM_SIZE:
            resize_island();
            return 0;
        case WM_DPICHANGED: {
            const auto suggested = reinterpret_cast<RECT*>(lparam);
            SetWindowPos(window_, nullptr, suggested->left, suggested->top,
                         suggested->right - suggested->left, suggested->bottom - suggested->top,
                         SWP_NOACTIVATE | SWP_NOZORDER);
            return 0;
        }
        case WM_SETTINGCHANGE:
        case WM_THEMECHANGED:
        case WM_SYSCOLORCHANGE:
            update_theme();
            return 0;
        case WM_DESTROY:
            close_xaml();
            return 0;
        case WM_NCDESTROY: {
            const HWND window = window_;
            deactivate_update_target();
            discard_pending_update_results();
            SetWindowLongPtrW(window, GWLP_USERDATA, 0);
            const LRESULT result = DefWindowProcW(window, message, wparam, lparam);
            window_ = nullptr;
            return result;
        }
        default:
            return DefWindowProcW(window_, message, wparam, lparam);
    }
}

void SettingsWindow::initialize_xaml_island() {
    xaml_source_ = Hosting::DesktopWindowXamlSource();
    xaml_source_.Initialize(winrt::Microsoft::UI::GetWindowIdFromWindow(window_));
    island_window_ = winrt::Microsoft::UI::GetWindowFromWindowId(
        xaml_source_.SiteBridge().WindowId());
    resize_island();
}

void SettingsWindow::build_page() {
    shell_ = load_xaml_resource(IDR_SETTINGS_PAGE_XAML).as<Grid>();

    model_path_ = named<TextBox>(shell_, L"ModelPath");
    model_path_.Text(to_hstring(configuration_.model_path));
    model_path_.TextChanged(
        [this](const auto&, const auto&) { update_model_path_save_state(); });
    browse_model_button_ = named<Button>(shell_, L"BrowseModelButton");
    browse_model_button_.Click([this](const auto&, const auto&) { browse_model_file(); });
    save_model_button_ = named<Button>(shell_, L"SaveModelButton");
    save_model_button_.Click([this](const auto&, const auto&) { save_model_path(); });
    model_note_ = named<TextBlock>(shell_, L"ModelNote");

    const auto active_row = named<Grid>(shell_, L"ActiveDeviceRow");
    const auto& active = configuration_.active_device;
    const std::u16string active_name =
        !active.description.empty() ? active.description
                                    : (!active.name.empty() ? active.name : active.device_id);
    const auto active_name_text = to_hstring(active_name);
    active_device_status_ = named<TextBlock>(shell_, L"ActiveDeviceStatus");
    active_device_status_.Text(active_name_text);
    std::u16string active_state = u"使用中 · ";
    active_state += backend_label(active.backend);
    named<TextBlock>(shell_, L"ActiveBackend").Text(to_hstring(active_state));

    ToolTip active_tooltip;
    StackPanel tooltip_content;
    tooltip_content.Spacing(4);
    tooltip_content.Children().Append(
        make_text(active_name_text.c_str(), body_text_size, FontWeights::SemiBold()));
    std::u16string backend_detail = u"後端：";
    backend_detail += backend_label(active.backend);
    const auto backend_detail_text = to_hstring(backend_detail);
    tooltip_content.Children().Append(make_text(backend_detail_text.c_str(), caption_text_size));
    if (active.backend != LLAVON_SETTINGS_BACKEND_CPU && active.memory_total != 0) {
        std::wostringstream memory;
        memory << L"顯示記憶體：" << std::fixed << std::setprecision(1)
               << static_cast<double>(active.memory_total) / 1024.0 / 1024.0 / 1024.0
               << L" GB";
        tooltip_content.Children().Append(make_text(memory.str().c_str(), caption_text_size));
    }
    if (!active.device_id.empty()) {
        const std::u16string device_id = u"裝置 ID：" + active.device_id;
        const auto device_id_text = to_hstring(device_id);
        tooltip_content.Children().Append(make_text(device_id_text.c_str(), caption_text_size));
    }
    const wchar_t* offload = configuration_.gpu_offload ? L"GPU offload：啟用"
                                                        : L"GPU offload：未啟用";
    tooltip_content.Children().Append(make_text(offload, caption_text_size));
    if (configuration_.fell_back_to_cpu) {
        tooltip_content.Children().Append(
            make_text(L"偏好裝置目前無法使用，已改用 CPU。", caption_text_size));
    }
    active_tooltip.Content(tooltip_content);
    ToolTipService::SetToolTip(active_row, active_tooltip);
    active_row.PointerEntered([active_tooltip](const auto&, const auto&) {
        active_tooltip.IsOpen(true);
    });
    active_row.PointerExited([active_tooltip](const auto&, const auto&) {
        active_tooltip.IsOpen(false);
    });

    inference_options_.clear();
    inference_options_.push_back(InferenceDeviceOption{
        .backend = LLAVON_SETTINGS_BACKEND_AUTO, .name = u"自動選擇",
    });
    inference_options_.push_back(InferenceDeviceOption{
        .backend = LLAVON_SETTINGS_BACKEND_CPU,
        .device_type = LLAVON_SETTINGS_DEVICE_CPU, .name = u"CPU",
    });
    for (const auto& device : configuration_.devices) {
        if (device.backend == LLAVON_SETTINGS_BACKEND_CUDA ||
            device.backend == LLAVON_SETTINGS_BACKEND_VULKAN) {
            inference_options_.push_back(device);
        }
    }
    bool selected_device_available =
        configuration_.selected_backend == LLAVON_SETTINGS_BACKEND_AUTO ||
        configuration_.selected_backend == LLAVON_SETTINGS_BACKEND_CPU;
    for (const auto& option : inference_options_) {
        if (option.backend == configuration_.selected_backend &&
            option.device_id == configuration_.selected_device_id) {
            selected_device_available = true;
            break;
        }
    }
    if (!selected_device_available) {
        inference_options_.push_back(InferenceDeviceOption{
            .backend = configuration_.selected_backend,
            .device_id = configuration_.selected_device_id,
            .name = std::u16string(u"原設定：") + backend_label(configuration_.selected_backend) +
                    u" / " + configuration_.selected_device_id + u"（目前不可用）",
        });
    }
    inference_device_ = named<ComboBox>(shell_, L"InferenceDevice");
    inference_device_.Items().Append(winrt::box_value(L"自動選擇（建議）"));
    inference_device_.Items().Append(winrt::box_value(L"CPU"));
    for (std::size_t index = 2; index < inference_options_.size(); ++index) {
        const auto& option = inference_options_[index];
        const bool unavailable = !selected_device_available &&
                                 option.backend == configuration_.selected_backend &&
                                 option.device_id == configuration_.selected_device_id;
        inference_device_.Items().Append(winrt::box_value(
            to_hstring(unavailable ? option.name : device_label(option))));
    }
    std::int32_t selected_index = 0;
    for (std::size_t index = 0; index < inference_options_.size(); ++index) {
        const auto& option = inference_options_[index];
        if (option.backend == configuration_.selected_backend &&
            option.device_id == configuration_.selected_device_id) {
            selected_index = static_cast<std::int32_t>(index);
            break;
        }
    }
    inference_device_.SelectedIndex(selected_index);
    inference_device_.SelectionChanged(
        [this](const auto&, const auto&) { update_inference_save_state(); });
    save_inference_button_ = named<Button>(shell_, L"SaveInferenceButton");
    save_inference_button_.Click([this](const auto&, const auto&) { save_inference_setting(); });
    note_ = named<TextBlock>(shell_, L"InferenceNote");

    const auto full_width_toggle = named<ToggleSwitch>(shell_, L"FullWidthToggle");
    auto saved_full_width =
        std::make_shared<bool>(configuration_.shift_space_width_toggle_enabled);
    auto updating_full_width = std::make_shared<bool>(false);
    full_width_toggle.IsOn(*saved_full_width);
    const auto full_width_note = named<TextBlock>(shell_, L"FullWidthNote");
    full_width_toggle.Toggled(
        [this, full_width_note, saved_full_width, updating_full_width](
            const auto& sender, const auto&) {
            if (*updating_full_width) return;
            const auto toggle = sender.template as<ToggleSwitch>();
            const bool enabled = toggle.IsOn();
            if (enabled == *saved_full_width) return;
            const std::int32_t result = configuration_.save_width_toggle_callback
                ? configuration_.save_width_toggle_callback(
                      configuration_.save_width_toggle_context, enabled ? 1 : 0)
                : ERROR_INVALID_FUNCTION;
            if (result == ERROR_SUCCESS) {
                *saved_full_width = enabled;
                full_width_note.Text(L"已儲存；切回輸入中的應用程式後套用。");
            } else {
                *updating_full_width = true;
                toggle.IsOn(*saved_full_width);
                *updating_full_width = false;
                full_width_note.Text(L"無法儲存全形設定，請稍後再試。");
            }
            full_width_note.Visibility(Visibility::Visible);
        });

    custom_names_panel_ = named<StackPanel>(shell_, L"CustomNamesPanel");
    add_custom_name_button_ = named<Button>(shell_, L"AddCustomNameButton");
    add_custom_name_button_.Click([this](const auto&, const auto&) { add_custom_name_row(); });
    save_custom_names_button_ = named<Button>(shell_, L"SaveCustomNamesButton");
    save_custom_names_button_.Click([this](const auto&, const auto&) { save_custom_names(); });
    custom_names_note_ = named<TextBlock>(shell_, L"CustomNamesNote");
    saved_custom_names_.clear();
    saved_custom_names_.reserve(configuration_.custom_names.size());
    for (const auto& custom_name : configuration_.custom_names) {
        saved_custom_names_.push_back(CustomNameEntry{
            .name = custom_name.name, .readings = custom_name.readings,
        });
    }
    if (configuration_.custom_names.empty()) {
        add_custom_name_row();
    } else {
        for (const auto& custom_name : configuration_.custom_names) {
            add_custom_name_row(custom_name.name, custom_name.readings);
        }
    }

    pending_summary_ = named<TextBlock>(shell_, L"PendingSummary");
    set_pending_count(configuration_.training_items.size());
    lora_note_ = named<TextBlock>(shell_, L"LoraNote");
    refresh_protection_controls();
    const auto recording = named<CheckBox>(shell_, L"EncryptedRecording");
    recording.Checked([this](const auto&, const auto&) {
        if (updating_protection_) return;
        std::size_t status = 0;
        const auto callback = configuration_.protection_callback;
        if (!callback || callback(configuration_.protection_context,
                LLAVON_PROTECTION_STATUS, nullptr, &status) != ERROR_SUCCESS) {
            refresh_protection_controls();
            return;
        }
        if ((status & 1) == 0) {
            refresh_protection_controls();
            show_password_dialog(true, [this](const char16_t* password) {
                std::size_t result = 0;
                const bool ok = configuration_.protection_callback(configuration_.protection_context,
                    LLAVON_PROTECTION_SETUP, password, &result) == ERROR_SUCCESS;
                refresh_protection_controls();
                return ok;
            });
        } else {
            callback(configuration_.protection_context, LLAVON_PROTECTION_ENABLE, nullptr, &status);
            refresh_protection_controls();
        }
    });
    recording.Unchecked([this](const auto&, const auto&) {
        if (updating_protection_) return;
        std::size_t result = 0;
        if (configuration_.protection_callback) configuration_.protection_callback(
            configuration_.protection_context, LLAVON_PROTECTION_DISABLE, nullptr, &result);
        refresh_protection_controls();
    });
    named<Button>(shell_, L"LoraButton").Click(
        [this](const auto&, const auto&) {
            const auto report_error = [this] {
                lora_note_.Text(L"無法開啟訓練設定，請稍後再試。");
                lora_note_.Visibility(Visibility::Visible);
                if (lora_dialog_timer_) lora_dialog_timer_.Stop();
                lora_dialog_timer_ = nullptr;
            };
            try {
                show_lora_training_dialog();
                lora_note_.Visibility(Visibility::Collapsed);
            } catch (const winrt::hresult_error& error) {
                OutputDebugStringW((L"[settings-ui] unable to open LoRA dialog: " +
                                    std::wstring(error.message()) + L"\n").c_str());
                report_error();
            } catch (const std::exception& error) {
                OutputDebugStringW((L"[settings-ui] unable to open LoRA dialog: " +
                                    std::wstring(winrt::to_hstring(error.what())) + L"\n").c_str());
                report_error();
            } catch (...) {
                OutputDebugStringW(L"[settings-ui] unable to open LoRA dialog: unknown error\n");
                report_error();
            }
        });

    const std::wstring build_label =
        L"目前：" + build_identity(UpdateChecker::installed_version(),
                                    UpdateChecker::installed_build_number(),
                                    UpdateChecker::installed_commit());
    named<TextBlock>(shell_, L"BuildLabel").Text(build_label);
    update_button_ = named<Button>(shell_, L"UpdateButton");
    update_button_.Click([this](const auto&, const auto&) { begin_update_check(); });
    update_download_ = named<HyperlinkButton>(shell_, L"UpdateDownload");
    update_status_ = named<TextBlock>(shell_, L"UpdateStatus");
    xaml_source_.Content(shell_);
}

void SettingsWindow::refresh_protection_controls() {
    std::size_t status = 0;
    const auto callback = configuration_.protection_callback;
    const bool ok = callback && callback(configuration_.protection_context,
        LLAVON_PROTECTION_STATUS, nullptr, &status) == ERROR_SUCCESS;
    updating_protection_ = true;
    named<CheckBox>(shell_, L"EncryptedRecording").IsChecked(ok && (status & 2) != 0);
    named<CheckBox>(shell_, L"EncryptedRecording").IsEnabled(ok);
    named<TextBlock>(shell_, L"PasswordStatus").Text(!ok ? L"無法讀取密碼狀態" :
        ((status & 1) != 0 ? L"密碼已設定" : L"密碼未設定"));
    updating_protection_ = false;
}

void SettingsWindow::show_password_dialog(bool setup, std::function<bool(const char16_t*)> action,
                                         bool clean_datasets) {
    ContentDialog dialog;
    dialog.XamlRoot(shell_.XamlRoot());
    dialog.RequestedTheme(shell_.ActualTheme());
    dialog.Title(winrt::box_value(setup ? L"設定密碼" : L"輸入密碼以繼續"));
    dialog.PrimaryButtonText(setup ? L"設定並開啟" : L"繼續");
    dialog.CloseButtonText(L"取消");
    StackPanel content;
    content.Spacing(12);
    content.Width(400);
    auto description = make_text(setup
        ? L"設定密碼以保護對話資料，如果不知道要設什麼建議 0000。"
        : L"請輸入密碼以解密對話資料。", body_text_size);
    description.TextWrapping(TextWrapping::Wrap);
    content.Children().Append(description);
    auto note = make_text(L"密碼不會被系統保存，忘記將無法還原。", caption_text_size);
    note.TextWrapping(TextWrapping::Wrap);
    note.Visibility(setup ? Visibility::Visible : Visibility::Collapsed);
    content.Children().Append(note);
    PasswordBox password;
    password.PlaceholderText(L"密碼");
    content.Children().Append(password);
    PasswordBox confirmation;
    confirmation.PlaceholderText(L"再次輸入密碼");
    confirmation.Visibility(setup ? Visibility::Visible : Visibility::Collapsed);
    content.Children().Append(confirmation);
    struct PasswordDialogState {
        bool setup;
        bool confirming_reset = false;
        bool input_enabled = true;
        winrt::hstring previous_status;
    };
    auto dialog_state = std::make_shared<PasswordDialogState>(PasswordDialogState{setup});
    Button reset;
    reset.Content(winrt::box_value(L"忘記密碼，清除所有對話資料"));
    reset.HorizontalAlignment(HorizontalAlignment::Right);
    reset.Visibility(setup ? Visibility::Collapsed : Visibility::Visible);
    TextBlock status;
    status.TextWrapping(TextWrapping::Wrap);
    content.Children().Append(status);
    content.Children().Append(reset);
    reset.Click([weak_dialog = winrt::make_weak(dialog), dialog_state, password,
                 confirmation, description, note, status](const auto& sender, const auto&) {
        const auto dialog = weak_dialog.get();
        if (!dialog) return;
        dialog_state->confirming_reset = true;
        dialog_state->input_enabled = dialog.IsPrimaryButtonEnabled();
        dialog_state->previous_status = status.Text();
        dialog.Title(winrt::box_value(L"清除所有對話資料？"));
        dialog.PrimaryButtonText(L"確認清除");
        dialog.CloseButtonText(L"返回");
        dialog.IsPrimaryButtonEnabled(true);
        description.Text(L"所有對話記錄及訓練暫存資料將永久刪除，無法復原。LoRA 模型檔及訓練歷程會保留。清除後可重新設定密碼。");
        password.Password(L"");
        confirmation.Password(L"");
        password.Visibility(Visibility::Collapsed);
        confirmation.Visibility(Visibility::Collapsed);
        note.Visibility(Visibility::Collapsed);
        sender.as<Button>().Visibility(Visibility::Collapsed);
        status.Text(L"");
    });
    dialog.CloseButtonClick([dialog_state, password, confirmation, description, note, reset, status](
        const auto& sender, const ContentDialogButtonClickEventArgs& args) {
        if (!dialog_state->confirming_reset) return;
        args.Cancel(true);
        dialog_state->confirming_reset = false;
        const auto dialog = sender.as<ContentDialog>();
        dialog.Title(winrt::box_value(dialog_state->setup ? L"設定密碼" : L"輸入密碼以繼續"));
        dialog.PrimaryButtonText(dialog_state->setup ? L"設定並開啟" : L"繼續");
        dialog.CloseButtonText(L"取消");
        dialog.IsPrimaryButtonEnabled(dialog_state->input_enabled);
        description.Text(dialog_state->setup
            ? L"設定密碼以保護對話資料，如果不知道要設什麼建議 0000。"
            : L"請輸入密碼以解密對話資料。");
        password.Visibility(Visibility::Visible);
        confirmation.Visibility(dialog_state->setup ? Visibility::Visible : Visibility::Collapsed);
        note.Visibility(dialog_state->setup ? Visibility::Visible : Visibility::Collapsed);
        reset.Visibility(dialog_state->setup ? Visibility::Collapsed : Visibility::Visible);
        status.Text(dialog_state->previous_status);
        password.Focus(FocusState::Programmatic);
    });
    dialog.Content(content);
    dialog.Opened([this, setup, clean_datasets, status, password](const auto& sender, const auto&) {
        password.Focus(FocusState::Programmatic);
        if (!setup && !clean_datasets) return;
        std::size_t removed = 0;
        const auto callback = configuration_.protection_callback;
        const auto result = callback ? callback(configuration_.protection_context,
            LLAVON_PROTECTION_CLEANUP, nullptr, &removed) : ERROR_INVALID_FUNCTION;
        if (result != ERROR_SUCCESS) {
            status.Text(L"無法清除明文暫存檔，目前不能繼續。請結束訓練並確認檔案權限後重試。");
            sender.as<ContentDialog>().IsPrimaryButtonEnabled(false);
        } else if (removed != 0) {
            status.Text(L"偵測到明文暫存檔，已刪除 " + std::to_wstring(removed) +
                        L" 個檔案；這些暫存資料不會再用於訓練。");
        }
    });
    dialog.PrimaryButtonClick([this, dialog_state, password, confirmation, description, note, reset, status, action = std::move(action)](
        const auto& sender, const ContentDialogButtonClickEventArgs& args) {
        if (dialog_state->confirming_reset) {
            args.Cancel(true);
            const auto dialog = sender.as<ContentDialog>();
            std::size_t result = 0;
            const auto callback = configuration_.protection_callback;
            if (!callback || callback(configuration_.protection_context,
                    LLAVON_PROTECTION_RESET, nullptr, &result) != ERROR_SUCCESS) {
                status.Text(L"清除未完成，請先結束訓練並確認檔案權限後重試。LoRA 模型檔不會被刪除。");
                return;
            }
            if (const auto close = close_lora_dialog_) close();
            configuration_.training_items.clear();
            set_pending_count(0);
            refresh_protection_controls();
            password.Password(L"");
            confirmation.Password(L"");
            confirmation.Visibility(Visibility::Visible);
            dialog_state->setup = true;
            dialog_state->confirming_reset = false;
            dialog.Title(winrt::box_value(L"設定密碼"));
            dialog.PrimaryButtonText(L"設定並開啟");
            dialog.IsPrimaryButtonEnabled(true);
            dialog.CloseButtonText(L"取消");
            password.Visibility(Visibility::Visible);
            note.Visibility(Visibility::Visible);
            reset.Visibility(Visibility::Collapsed);
            description.Text(L"設定密碼以保護對話資料，如果不知道要設什麼建議 0000。");
            reset.Content(winrt::box_value(L"忘記密碼，清除所有對話資料"));
            status.Text(L"已清除所有對話資料，LoRA 模型檔已保留。請設定新密碼。");
            password.Focus(FocusState::Programmatic);
            return;
        }
        auto secret = to_utf16(password.Password());
        auto repeated = to_utf16(confirmation.Password());
        const bool valid = !dialog_state->confirming_reset && !secret.empty() &&
            (!dialog_state->setup || secret == repeated);
        bool ok = false;
        if (valid) {
            try {
                if (dialog_state->setup) {
                    std::size_t result = 0;
                    ok = configuration_.protection_callback && configuration_.protection_callback(
                        configuration_.protection_context, LLAVON_PROTECTION_SETUP,
                        secret.c_str(), &result) == ERROR_SUCCESS;
                    refresh_protection_controls();
                } else {
                    ok = action(secret.c_str());
                }
            } catch (...) {}
        }
        if (!secret.empty()) SecureZeroMemory(secret.data(), secret.size() * sizeof(char16_t));
        if (!repeated.empty()) SecureZeroMemory(repeated.data(), repeated.size() * sizeof(char16_t));
        password.Password(L"");
        confirmation.Password(L"");
        if (!ok) {
            args.Cancel(true);
            status.Text(!valid ? L"請輸入密碼，並確認兩次輸入相同。" :
                L"密碼錯誤或資料處理失敗，請重試。");
        }
    });
    dialog.Closed([password, confirmation](const auto&, const auto&) {
        password.Password(L"");
        confirmation.Password(L"");
    });
    (void)dialog.ShowAsync();
}

bool SettingsWindow::load_training_items(const char16_t* password) {
    configuration_.training_items.clear();
    const auto callback = configuration_.refresh_training_items_callback;
    std::size_t count = 0;
    if (!callback || callback(configuration_.refresh_training_items_context,
            nullptr, 0, &count, password) != ERROR_SUCCESS) return false;
    std::vector<llavon_settings_training_item> items(count);
    if (count != 0 && callback(configuration_.refresh_training_items_context,
            items.data(), items.size(), &count, password) != ERROR_SUCCESS) return false;
    for (std::size_t index = 0; index < count; ++index) {
        const auto& item = items[index];
        configuration_.training_items.push_back(TrainingDataOption{
            .event_id = item.event_id, .context = item.context, .answer = item.answer,
            .reading = item.reading, .revice = item.revice != 0,
        });
    }
    if (configuration_.protection_callback) configuration_.protection_callback(
        configuration_.protection_context, LLAVON_PROTECTION_CLEAR_VIEW, nullptr, &count);
    return true;
}

void SettingsWindow::show_lora_training_dialog() {
    if (lora_dialog_open_) return;

    if (!load_training_items()) throw std::runtime_error("unable to load training metadata");

    struct DialogState {
        Grid overlay{nullptr};
        ContentControl content{nullptr};
        TextBlock title{nullptr};
        Button primary_button{nullptr};
        Button secondary_button{nullptr};
        std::vector<TrainingDataOption> items;
        std::vector<bool> deleted_items;
        std::size_t active_count = 0;
        std::size_t current_page = 0;
        ListView training_items{nullptr};
        TextBlock training_summary{nullptr};
        TextBlock delete_status{nullptr};
        TextBlock page_summary{nullptr};
        Button previous_page{nullptr};
        Button next_page{nullptr};
        Button training_data_button{nullptr};
        Button training_history_button{nullptr};
        StackPanel training_data_page{nullptr};
        ScrollViewer main_page{nullptr};
        bool selecting_training_data = false;
        TextBox rank{nullptr};
        TextBox alpha{nullptr};
        TextBox dropout{nullptr};
        TextBox batch_size{nullptr};
        TextBox gradient_accumulation{nullptr};
        TextBox epochs{nullptr};
        TextBox max_steps{nullptr};
        TextBox learning_rate{nullptr};
        TextBox weight_decay{nullptr};
        TextBox warmup_steps{nullptr};
        TextBox max_gradient_norm{nullptr};
        TextBox save_every{nullptr};
        TextBox seed{nullptr};
        TextBox max_sequence_length{nullptr};
        TextBox target_modules{nullptr};
        ComboBox device{nullptr};
        ComboBox dtype{nullptr};
        ToggleSwitch shuffle{nullptr};
        ProgressBar progress{nullptr};
        ProgressBar download_progress{nullptr};
        TextBlock status{nullptr};
        Border model_status_card{nullptr};
        FontIcon model_status_icon{nullptr};
        TextBlock model_status_title{nullptr};
        TextBlock model_status_detail{nullptr};
        Button check_model{nullptr};
        Button download_model{nullptr};
        Button cancel{nullptr};
        Button reload_model{nullptr};
        TextBlock reload_model_status{nullptr};
        TextBlock estimated_steps{nullptr};
        DispatcherTimer timer{nullptr};
        std::u16string output_model_path;
        bool model_reload_failed = false;
        bool model_available = false;
        bool busy = false;
        bool closed = false;
    };

    auto state = std::make_shared<DialogState>();
    state->items = configuration_.training_items;
    state->deleted_items.assign(state->items.size(), false);
    state->active_count = state->items.size();
    state->overlay = load_xaml_resource(IDR_LORA_DIALOG_XAML).as<Grid>();
    const auto dialog_root = state->overlay.as<FrameworkElement>();
    state->content = named<ContentControl>(dialog_root, L"DialogContent");
    state->title = named<TextBlock>(dialog_root, L"DialogTitle");
    state->primary_button = named<Button>(dialog_root, L"DialogPrimaryButton");
    state->secondary_button = named<Button>(dialog_root, L"DialogSecondaryButton");
    state->main_page = named<ScrollViewer>(dialog_root, L"MainScroll");
    state->model_status_card = named<Border>(dialog_root, L"ModelStatusBorder");
    state->model_status_icon = named<FontIcon>(dialog_root, L"ModelStatusIcon");
    state->model_status_title = named<TextBlock>(dialog_root, L"ModelStatusTitle");
    state->model_status_detail = named<TextBlock>(dialog_root, L"ModelStatusDetail");
    state->download_progress = named<ProgressBar>(dialog_root, L"DownloadProgress");
    state->check_model = named<Button>(dialog_root, L"CheckModelButton");
    state->download_model = named<Button>(dialog_root, L"DownloadModelButton");
    state->cancel = named<Button>(dialog_root, L"CancelModelButton");
    state->training_summary = named<TextBlock>(dialog_root, L"TrainingSummary");
    state->training_data_button = named<Button>(dialog_root, L"TrainingDataButton");
    state->training_history_button = named<Button>(dialog_root, L"TrainingHistoryButton");

    state->training_history_button.Click([this, state](const auto&, const auto&) {
        std::size_t count = 0;
        const auto callback = configuration_.get_lora_history_callback;
        const std::int32_t count_result = callback
            ? callback(configuration_.get_lora_history_context, nullptr, 0, &count)
            : ERROR_INVALID_FUNCTION;
        std::vector<llavon_settings_lora_history_item> history(count);
        const std::int32_t load_result = count_result == ERROR_SUCCESS
            ? callback(configuration_.get_lora_history_context, history.data(),
                       history.size(), &count)
            : count_result;

        StackPanel content;
        content.Width(360);
        content.Spacing(12);
        content.Children().Append(make_text(
            L"訓練歷程", 16, FontWeights::SemiBold()));

        StackPanel timeline;
        timeline.Spacing(0);
        const bool dark = system_uses_dark_theme();
        const auto accent = dark ? solid_brush(96, 205, 255)
                                 : solid_brush(0, 120, 212);
        const auto muted = dark ? solid_brush(96, 96, 96)
                                : solid_brush(190, 190, 190);

        const auto append_timeline_item = [&](const std::wstring& title,
                                               const std::wstring& detail,
                                               bool last) {
            Grid row;
            row.ColumnDefinitions().Append(ColumnDefinition{});
            row.ColumnDefinitions().GetAt(0).Width(GridLength{24, GridUnitType::Pixel});
            row.ColumnDefinitions().Append(ColumnDefinition{});
            row.ColumnDefinitions().GetAt(1).Width(GridLength{1, GridUnitType::Star});

            StackPanel rail;
            rail.HorizontalAlignment(HorizontalAlignment::Center);
            Border node;
            node.Width(11);
            node.Height(11);
            node.CornerRadius(CornerRadius{6, 6, 6, 6});
            node.Background(accent);
            rail.Children().Append(node);
            if (!last) {
                Border line;
                line.Width(2);
                line.Height(detail.empty() ? 31 : 47);
                line.Background(muted);
                rail.Children().Append(line);
            }
            Grid::SetColumn(rail, 0);
            row.Children().Append(rail);

            StackPanel labels;
            labels.Margin(Thickness{8.0, 0.0, 0.0, last ? 0.0 : 10.0});
            labels.Children().Append(make_text(
                title.c_str(), body_text_size, FontWeights::SemiBold()));
            if (!detail.empty()) {
                labels.Children().Append(make_text(
                    detail.c_str(), caption_text_size));
            }
            Grid::SetColumn(labels, 1);
            row.Children().Append(labels);
            timeline.Children().Append(row);
        };

        if (load_result != ERROR_SUCCESS || count == 0) {
            append_timeline_item(L"Base model", L"", false);
            append_timeline_item(L"尚無訓練紀錄", L"", true);
        } else {
            append_timeline_item(L"Base model", L"", false);
            for (std::size_t index = 0; index < count; ++index) {
                const auto& item = history[index];
                const std::wstring title = item.completed_at_utc
                    ? utc8_timestamp(item.completed_at_utc)
                    : std::wstring{};
                const std::wstring detail = std::format(
                    L"新增 {} 筆　累計 {} 筆　{} steps",
                    item.record_count, item.cumulative_record_count,
                    item.optimizer_steps);
                append_timeline_item(title, detail, index + 1 == count);
            }
        }
        content.Children().Append(timeline);
        ScrollViewer scroller;
        scroller.MaxHeight(440);
        scroller.VerticalScrollBarVisibility(ScrollBarVisibility::Auto);
        scroller.Content(content);
        Flyout flyout;
        flyout.Content(scroller);
        flyout.Placement(FlyoutPlacementMode::BottomEdgeAlignedRight);
        flyout.ShowAt(state->training_history_button);
    });

    const auto refresh_training_count =
        std::make_shared<std::function<void()>>();
    if (state->items.empty()) {
        named<TextBlock>(dialog_root, L"EmptyTrainingItems").Visibility(Visibility::Visible);
        state->training_data_button.IsEnabled(false);
    } else {
        state->training_data_page =
            load_xaml_resource(IDR_LORA_SELECTION_XAML).as<StackPanel>();
        state->training_items =
            named<ListView>(state->training_data_page, L"TrainingItems");
        const bool dark_selection = system_uses_dark_theme();
        named<Border>(state->training_data_page, L"TrainingItemsBorder").BorderBrush(
            dark_selection ? solid_brush(64, 64, 64) : solid_brush(225, 225, 225));
        state->training_items.Resources().Insert(
            winrt::box_value(L"ListViewItemBackground"),
            dark_selection ? solid_brush(39, 39, 39) : solid_brush(243, 244, 246));
        state->page_summary = named<TextBlock>(state->training_data_page, L"PageSummary");
        state->delete_status = named<TextBlock>(
            state->training_data_page, L"DeleteStatus");
        state->previous_page =
            named<Button>(state->training_data_page, L"PreviousPageButton");
        state->next_page = named<Button>(state->training_data_page, L"NextPageButton");

        const auto render_page = std::make_shared<std::function<void()>>();
        *render_page = [this, state, dark_selection, refresh_training_count] {
            state->training_items.Items().Clear();
            const std::size_t begin = state->current_page * training_page_size;
            const std::size_t end = std::min(
                begin + training_page_size, state->items.size());
            for (std::size_t index = begin; index < end; ++index) {
                const auto& item = state->items[index];
                Grid row;
                ColumnDefinition action_column;
                action_column.Width(GridLength{44, GridUnitType::Pixel});
                row.ColumnDefinitions().Append(action_column);
                row.ColumnDefinitions().Append(ColumnDefinition{});
                Button remove;
                remove.Width(32);
                remove.Height(32);
                remove.Padding(Thickness{0});
                remove.VerticalAlignment(VerticalAlignment::Center);
                FontIcon icon;
                icon.FontFamily(FontFamily(L"Segoe Fluent Icons"));
                icon.Glyph(L"\uE74D");
                icon.FontSize(16);
                remove.Content(icon);
                Automation::AutomationProperties::SetName(
                    remove, L"刪除訓練資料");
                ToolTipService::SetToolTip(remove, winrt::box_value(L"刪除訓練資料"));
                Grid::SetColumn(remove, 0);
                row.Children().Append(remove);
                FrameworkElement content{nullptr};
                try {
                    auto root = load_xaml_resource(
                        IDR_LORA_TRAINING_ITEM_XAML).as<Grid>();
                    render_training_item(item, root, root, dark_selection);
                    Automation::AutomationProperties::SetName(
                        root, to_hstring(item.answer));
                    content = root;
                } catch (...) {
                    TextBlock fallback;
                    fallback.Text(to_hstring(item.answer));
                    fallback.FontSize(18);
                    fallback.Padding(Thickness{12, 10, 12, 10});
                    fallback.TextWrapping(TextWrapping::WrapWholeWords);
                    content = fallback;
                }
                Grid::SetColumn(content, 1);
                row.Children().Append(content);
                if (state->deleted_items[index]) {
                    row.Opacity(0.4);
                    remove.IsEnabled(false);
                }
                remove.Click([this, state, index, row, remove, refresh_training_count]
                    (const auto&, const auto&) {
                    if (state->closed || state->deleted_items[index]) return;
                    const auto callback = configuration_.delete_training_item_callback;
                    const auto result = callback
                        ? callback(configuration_.delete_training_item_context,
                                   state->items[index].event_id.c_str())
                        : ERROR_INVALID_FUNCTION;
                    if (result != ERROR_SUCCESS) {
                        state->delete_status.Text(L"刪除失敗，請稍後再試。");
                        state->delete_status.Visibility(Visibility::Visible);
                        return;
                    }
                    state->delete_status.Visibility(Visibility::Collapsed);
                    state->deleted_items[index] = true;
                    --state->active_count;
                    row.Opacity(0.4);
                    remove.IsEnabled(false);
                    (*refresh_training_count)();
                });
                state->training_items.Items().Append(row);
            }
            const std::size_t page_count =
                (state->items.size() + training_page_size - 1) /
                training_page_size;
            state->page_summary.Text(
                L"第 " + std::to_wstring(state->current_page + 1) + L" / " +
                std::to_wstring(page_count) + L" 頁（" +
                std::to_wstring(begin + 1) + L"–" + std::to_wstring(end) +
                L"）");
            state->previous_page.IsEnabled(state->current_page != 0);
            state->next_page.IsEnabled(state->current_page + 1 < page_count);
        };
        state->previous_page.Click(
            [state, render_page](const auto&, const auto&) {
                if (state->current_page != 0) --state->current_page;
                (*render_page)();
            });
        state->next_page.Click(
            [state, render_page](const auto&, const auto&) {
                const std::size_t page_count =
                    (state->items.size() + training_page_size - 1) /
                    training_page_size;
                if (state->current_page + 1 < page_count) ++state->current_page;
                (*render_page)();
            });
        (*render_page)();
        state->training_data_button.Click([this, state, render_page](const auto&, const auto&) {
            show_password_dialog(false, [this, state, render_page](const char16_t* password) {
                if (state->closed || !load_training_items(password)) return false;
                for (std::size_t index = 0; index < state->items.size(); ++index) {
                    if (state->deleted_items[index]) continue;
                    auto& item = state->items[index];
                    const auto found = std::find_if(configuration_.training_items.begin(),
                        configuration_.training_items.end(), [&](const auto& loaded) {
                            return loaded.event_id == item.event_id;
                    });
                    if (found == configuration_.training_items.end()) return false;
                    item = *found;
                }
                configuration_.training_items.clear();
                (*render_page)();
                state->selecting_training_data = true;
                state->content.Content(state->training_data_page);
                state->title.Text(L"檢視訓練資料");
                state->primary_button.Content(winrt::box_value(L"完成"));
                Grid::SetColumnSpan(state->primary_button, 2);
                state->secondary_button.Visibility(Visibility::Collapsed);
                state->primary_button.IsEnabled(true);
                return true;
            });
        });
    }

    state->rank = named<TextBox>(dialog_root, L"Rank");
    state->alpha = named<TextBox>(dialog_root, L"Alpha");
    state->dropout = named<TextBox>(dialog_root, L"Dropout");
    state->batch_size = named<TextBox>(dialog_root, L"BatchSize");
    state->gradient_accumulation = named<TextBox>(dialog_root, L"GradientAccumulation");
    state->epochs = named<TextBox>(dialog_root, L"Epochs");
    state->max_steps = named<TextBox>(dialog_root, L"MaxSteps");
    state->learning_rate = named<TextBox>(dialog_root, L"LearningRate");
    state->weight_decay = named<TextBox>(dialog_root, L"WeightDecay");
    state->warmup_steps = named<TextBox>(dialog_root, L"WarmupSteps");
    state->max_gradient_norm = named<TextBox>(dialog_root, L"MaxGradientNorm");
    state->save_every = named<TextBox>(dialog_root, L"SaveEvery");
    state->seed = named<TextBox>(dialog_root, L"Seed");
    state->max_sequence_length = named<TextBox>(dialog_root, L"MaxSequenceLength");
    state->target_modules = named<TextBox>(dialog_root, L"TargetModules");
    state->device = named<ComboBox>(dialog_root, L"Device");
    state->device.SelectedIndex(0);
    state->dtype = named<ComboBox>(dialog_root, L"DType");
    state->dtype.SelectedIndex(0);
    state->shuffle = named<ToggleSwitch>(dialog_root, L"Shuffle");
    state->progress = named<ProgressBar>(dialog_root, L"TrainingProgress");
    state->status = named<TextBlock>(dialog_root, L"Status");
    state->reload_model = named<Button>(dialog_root, L"ReloadModelButton");
    state->reload_model_status = named<TextBlock>(dialog_root, L"ReloadModelStatus");
    state->estimated_steps = named<TextBlock>(dialog_root, L"EstimatedSteps");

    const auto refresh_estimated_steps = [state] {
        const auto parse_integer = [](const TextBox& box) -> std::optional<std::int64_t> {
            try {
                const std::wstring value = box.Text().c_str();
                std::size_t consumed = 0;
                const auto parsed = std::stoll(value, &consumed);
                return consumed == value.size()
                    ? std::optional<std::int64_t>(parsed)
                    : std::nullopt;
            } catch (...) {
                return std::nullopt;
            }
        };
        const auto batch_size = parse_integer(state->batch_size);
        const auto accumulation = parse_integer(state->gradient_accumulation);
        const auto epochs = parse_integer(state->epochs);
        const auto max_steps = parse_integer(state->max_steps);
        if (!batch_size || !accumulation || !epochs || !max_steps ||
            *batch_size <= 0 || *accumulation <= 0 || *epochs <= 0 ||
            (*max_steps < -1 || *max_steps == 0)) {
            return;
        }

        const std::uint64_t selected = static_cast<std::uint64_t>(state->active_count);
        const std::uint64_t batch = static_cast<std::uint64_t>(*batch_size);
        const std::uint64_t gradient = static_cast<std::uint64_t>(*accumulation);
        const std::uint64_t batches = (selected + batch - 1) / batch;
        const std::uint64_t updates = (batches + gradient - 1) / gradient;
        const std::uint64_t epoch_steps =
            updates * static_cast<std::uint64_t>(*epochs);
        const std::uint64_t estimated = *max_steps > 0
            ? std::min(epoch_steps, static_cast<std::uint64_t>(*max_steps))
            : epoch_steps;
        state->estimated_steps.Text(
            L"預計 steps：" + std::to_wstring(estimated));
    };

    const auto refresh_training_selection = [state, refresh_estimated_steps] {
        state->training_summary.Text(training_count_label(state->active_count));
        state->training_data_button.IsEnabled(state->active_count != 0);
        state->primary_button.IsEnabled(
            state->selecting_training_data ||
            (!state->busy && state->model_available && state->active_count != 0));
        refresh_estimated_steps();
    };
    *refresh_training_count = refresh_training_selection;
    for (const auto& field : {state->batch_size, state->gradient_accumulation,
                              state->epochs, state->max_steps}) {
        field.TextChanged([refresh_estimated_steps](const auto&, const auto&) {
            refresh_estimated_steps();
        });
    }
    refresh_training_selection();


    const auto request_model_action = [this, state](bool download) {
        const std::int32_t result = configuration_.lora_model_action_callback
            ? configuration_.lora_model_action_callback(
                  configuration_.lora_model_action_context, download ? 1 : 0)
            : ERROR_INVALID_FUNCTION;
        if (result != ERROR_SUCCESS) {
            state->status.Text(result == ERROR_BUSY
                ? L"另一個 LoRA 操作正在執行。"
                : L"無法啟動模型操作。");
        }
    };
    state->check_model.Click(
        [request_model_action](const auto&, const auto&) { request_model_action(false); });
    state->download_model.Click(
        [request_model_action](const auto&, const auto&) { request_model_action(true); });
    state->cancel.Click([this](const auto&, const auto&) {
        if (configuration_.cancel_lora_callback) {
            configuration_.cancel_lora_callback(configuration_.cancel_lora_context);
        }
    });
    const auto refresh_reload_result = [this, state] {
        const bool completed = !state->output_model_path.empty();
        const bool loaded = completed &&
            state->output_model_path == configuration_.model_path;
        state->reload_model.Visibility(completed && !loaded
            ? Visibility::Visible : Visibility::Collapsed);
        state->reload_model_status.Text(loaded
            ? L"載入成功" : L"模型載入失敗，請重試。");
        state->reload_model_status.Visibility(completed && (loaded || state->model_reload_failed)
            ? Visibility::Visible : Visibility::Collapsed);
    };
    state->reload_model.Click([this, state, refresh_reload_result](const auto&, const auto&) {
        if (state->output_model_path.empty()) return;
        const std::int32_t result = configuration_.save_model_path_callback
            ? configuration_.save_model_path_callback(
                  configuration_.save_model_path_context,
                  state->output_model_path.c_str())
            : ERROR_INVALID_FUNCTION;
        state->model_reload_failed = result != ERROR_SUCCESS;
        if (result == ERROR_SUCCESS) {
            configuration_.model_path = state->output_model_path;
            model_path_.Text(to_hstring(configuration_.model_path));
            update_model_path_save_state();
            state->secondary_button.Focus(FocusState::Programmatic);
        }
        refresh_reload_result();
    });

    const auto refresh_status = [this, state, refresh_training_selection, refresh_reload_result] {
        llavon_settings_lora_status status{};
        const std::int32_t result = configuration_.get_lora_status_callback
            ? configuration_.get_lora_status_callback(
                  configuration_.get_lora_status_context, &status)
            : ERROR_INVALID_FUNCTION;
        if (result != ERROR_SUCCESS) {
            state->status.Text(L"無法取得 LoRA 狀態。");
            return;
        }
        if (status.message && *status.message) {
            state->status.Text(to_hstring(std::u16string_view(status.message)));
        }
        const bool busy = status.stage == LLAVON_SETTINGS_LORA_CHECKING_MODEL ||
                          status.stage == LLAVON_SETTINGS_LORA_DOWNLOADING_MODEL ||
                          status.stage == LLAVON_SETTINGS_LORA_PREPARING_DATA ||
                          status.stage == LLAVON_SETTINGS_LORA_TRAINING ||
                          status.stage == LLAVON_SETTINGS_LORA_EXPORTING_MODEL;
        const bool checking_model =
            status.stage == LLAVON_SETTINGS_LORA_CHECKING_MODEL;
        const bool downloading_model =
            status.stage == LLAVON_SETTINGS_LORA_DOWNLOADING_MODEL;
        const bool training_busy =
            status.stage == LLAVON_SETTINGS_LORA_PREPARING_DATA ||
            status.stage == LLAVON_SETTINGS_LORA_TRAINING ||
            status.stage == LLAVON_SETTINGS_LORA_EXPORTING_MODEL;
        state->busy = busy;
        state->model_available = status.model_available != 0;

        state->progress.Visibility(
            training_busy ? Visibility::Visible : Visibility::Collapsed);
        state->progress.IsIndeterminate(
            training_busy && status.stage == LLAVON_SETTINGS_LORA_EXPORTING_MODEL);
        if (!state->progress.IsIndeterminate()) {
            state->progress.Value(std::clamp(status.progress, 0.0, 1.0) * 100.0);
        }

        state->download_progress.Visibility(
            checking_model || downloading_model
                ? Visibility::Visible
                : Visibility::Collapsed);
        state->download_progress.IsIndeterminate(checking_model);
        if (downloading_model) {
            const double progress = std::clamp(status.progress, 0.0, 1.0) * 100.0;
            state->download_progress.Value(progress);
            state->model_status_title.Text(
                L"正在下載模型… " + std::to_wstring(static_cast<int>(std::lround(progress))) +
                L"%");
            state->model_status_detail.Visibility(Visibility::Collapsed);
        } else if (checking_model) {
            state->model_status_title.Text(L"正在檢查模型…");
            state->model_status_detail.Visibility(Visibility::Collapsed);
        } else if (status.model_available && status.model_update_available) {
            state->model_status_title.Text(L"有可用更新");
            state->model_status_detail.Text(L"基礎模型已下載，可以開始訓練。");
            state->model_status_detail.Visibility(Visibility::Visible);
        } else if (status.model_available) {
            state->model_status_title.Text(L"模型已就緒");
            state->model_status_detail.Text(L"基礎模型已下載，可以開始訓練。");
            state->model_status_detail.Visibility(Visibility::Visible);
        } else {
            state->model_status_title.Text(L"尚未下載");
            state->model_status_detail.Text(L"下載模型後才能開始訓練。");
            state->model_status_detail.Visibility(Visibility::Visible);
        }

        state->model_status_icon.Glyph(
            status.model_available ? L"\uE73E" :
            (downloading_model || checking_model ? L"\uE896" : L"\uE118"));

        state->check_model.IsEnabled(!busy);
        state->download_model.IsEnabled(!busy);
        state->download_model.Content(winrt::box_value(
            status.model_available && status.model_update_available
                ? L"更新模型"
                : L"下載模型"));
        state->download_model.Visibility(
            !downloading_model &&
                    (!status.model_available || status.model_update_available)
                ? Visibility::Visible
                : Visibility::Collapsed);
        state->cancel.Visibility(
            downloading_model ? Visibility::Visible : Visibility::Collapsed);
        refresh_training_selection();
        const std::u16string output_model_path =
            status.stage == LLAVON_SETTINGS_LORA_COMPLETED && status.output_model_path
                ? status.output_model_path : u"";
        if (state->output_model_path != output_model_path) {
            state->output_model_path = output_model_path;
            state->model_reload_failed = false;
        }
        refresh_reload_result();
    };

    state->timer = DispatcherTimer();
    state->timer.Interval(std::chrono::milliseconds(500));
    state->timer.Tick([refresh_status](const auto&, const auto&) { refresh_status(); });
    refresh_status();
    state->timer.Start();
    lora_dialog_timer_ = state->timer;
    const auto close_dialog = [this, state] {
        if (state->closed) return;
        state->closed = true;
        state->timer.Stop();
        if (state->training_items) state->training_items.Items().Clear();
        state->items.clear();
        configuration_.training_items.clear();
        lora_dialog_timer_ = nullptr;
        const auto children = shell_.Children();
        std::uint32_t index = 0;
        if (children.IndexOf(state->overlay, index)) {
            children.RemoveAt(index);
        }
        children.GetAt(0).as<Control>().IsEnabled(true);
        named<Button>(shell_, L"LoraButton").Focus(FocusState::Programmatic);
        lora_dialog_open_ = false;
        close_lora_dialog_ = {};
    };
    close_lora_dialog_ = close_dialog;
    state->secondary_button.Click([close_dialog](const auto&, const auto&) {
        close_dialog();
    });
    state->overlay.KeyDown([close_dialog](
        const auto&, const winrt::Microsoft::UI::Xaml::Input::KeyRoutedEventArgs& args) {
        if (static_cast<int>(args.Key()) == VK_ESCAPE) {
            args.Handled(true);
            close_dialog();
        }
    });

    state->primary_button.Click(
        [this, state, refresh_training_selection](
            const auto&, const auto&) {
            if (state->selecting_training_data) {
                state->selecting_training_data = false;
                state->content.Content(state->main_page);
                state->title.Text(L"訓練個人化模型");
                state->primary_button.Content(winrt::box_value(L"開始訓練"));
                Grid::SetColumnSpan(state->primary_button, 1);
                state->secondary_button.Visibility(Visibility::Visible);
                refresh_training_selection();
                return;
            }
            try {
                const auto parse_integer = [](const TextBox& box) {
                    const std::wstring value = box.Text().c_str();
                    std::size_t consumed = 0;
                    const long long parsed = std::stoll(value, &consumed);
                    if (consumed != value.size() ||
                        parsed < std::numeric_limits<std::int32_t>::min() ||
                        parsed > std::numeric_limits<std::int32_t>::max()) {
                        throw std::invalid_argument("integer");
                    }
                    return static_cast<std::int32_t>(parsed);
                };
                const auto parse_real = [](const TextBox& box) {
                    const std::wstring value = box.Text().c_str();
                    std::size_t consumed = 0;
                    const double parsed = std::stod(value, &consumed);
                    if (consumed != value.size() || !std::isfinite(parsed)) {
                        throw std::invalid_argument("number");
                    }
                    return parsed;
                };

                llavon_settings_lora_options options{
                    .rank = parse_integer(state->rank),
                    .alpha = parse_real(state->alpha),
                    .dropout = parse_real(state->dropout),
                    .batch_size = parse_integer(state->batch_size),
                    .gradient_accumulation = parse_integer(state->gradient_accumulation),
                    .epochs = parse_integer(state->epochs),
                    .max_steps = parse_integer(state->max_steps),
                    .learning_rate = parse_real(state->learning_rate),
                    .weight_decay = parse_real(state->weight_decay),
                    .warmup_steps = parse_integer(state->warmup_steps),
                    .max_gradient_norm = parse_real(state->max_gradient_norm),
                    .save_every = parse_integer(state->save_every),
                    .device = state->device.SelectedIndex() == 1
                        ? LLAVON_SETTINGS_LORA_DEVICE_CUDA
                        : (state->device.SelectedIndex() == 2
                               ? LLAVON_SETTINGS_LORA_DEVICE_CPU
                               : LLAVON_SETTINGS_LORA_DEVICE_AUTO),
                    .seed = parse_integer(state->seed),
                    .shuffle = state->shuffle.IsOn() ? 1 : 0,
                    .max_sequence_length = parse_integer(state->max_sequence_length),
                    .dtype = nullptr,
                    .target_modules = nullptr,
                };
                const std::u16string dtype = state->dtype.SelectedIndex() == 1
                    ? u"bfloat16"
                    : u"float32";
                const std::u16string target_modules = to_utf16(state->target_modules.Text());
                options.dtype = dtype.c_str();
                options.target_modules = target_modules.c_str();

                if (options.rank <= 0 || options.alpha <= 0 ||
                    options.dropout < 0 || options.dropout >= 1 ||
                    options.batch_size <= 0 || options.gradient_accumulation <= 0 ||
                    options.epochs <= 0 || options.max_steps == 0 ||
                    options.max_steps < -1 ||
                    options.learning_rate <= 0 || options.weight_decay < 0 ||
                    options.warmup_steps < 0 || options.max_gradient_norm < 0 ||
                    options.save_every < 0 || options.max_sequence_length <= 1 ||
                    target_modules.empty()) {
                    throw std::invalid_argument("range");
                }

                std::vector<std::u16string> selected;
                for (std::size_t index = 0; index < state->items.size(); ++index) {
                    if (!state->deleted_items[index]) {
                        selected.push_back(state->items[index].event_id);
                    }
                }
                show_password_dialog(false, [this, state, options, dtype, target_modules, selected](const char16_t* password) mutable {
                    if (state->closed) return false;
                    options.dtype = dtype.c_str();
                    options.target_modules = target_modules.c_str();
                    std::vector<const char16_t*> selected_pointers;
                    for (const auto& event_id : selected) selected_pointers.push_back(event_id.c_str());
                    state->progress.IsIndeterminate(true);
                    state->progress.Visibility(Visibility::Visible);
                    state->status.Text(L"正在由 service 儲存資料選擇…");
                    const std::int32_t result = configuration_.start_lora_training_callback
                        ? configuration_.start_lora_training_callback(
                              configuration_.start_lora_training_context,
                              selected_pointers.data(), selected_pointers.size(), &options, password)
                        : ERROR_INVALID_FUNCTION;
                    if (result == ERROR_SUCCESS) {
                        state->primary_button.IsEnabled(false);
                        state->status.Text(L"訓練已啟動，關閉視窗後仍會在背景繼續。");
                    } else {
                        state->progress.Visibility(Visibility::Collapsed);
                        state->status.Text(L"無法儲存訓練資料選擇，請稍後再試。");
                    }
                    return result == ERROR_SUCCESS;
                }, true);
            } catch (...) {
                state->progress.Visibility(Visibility::Collapsed);
                state->status.Text(L"參數格式或範圍不正確，請檢查所有欄位。");
            }
        });

    state->overlay.Loaded([button = state->secondary_button](const auto&, const auto&) {
        button.Focus(FocusState::Programmatic);
    });
    shell_.Children().GetAt(0).as<Control>().IsEnabled(false);
    shell_.Children().Append(state->overlay);
    lora_dialog_open_ = true;
}

void SettingsWindow::add_custom_name_row(
    std::u16string name, std::vector<std::u16string> readings) {
    if (!custom_names_panel_) {
        return;
    }

    CustomNameRow row;
    row.container = load_xaml_resource(IDR_CUSTOM_NAME_ROW_XAML).as<Grid>();
    row.name = named<TextBox>(row.container, L"Name");
    row.name.Text(to_hstring(name));
    row.pronunciations = named<StackPanel>(row.container, L"Pronunciations");
    row.remove_button = named<Button>(row.container, L"RemoveButton");

    const Button remove_button = row.remove_button;
    const TextBox name_box = row.name;
    row.name.TextChanged([this, name_box](const auto&, const auto&) {
        refresh_custom_name_pronunciations(name_box);
    });
    row.remove_button.Click([this, remove_button](const auto&, const auto&) {
        remove_custom_name_row(remove_button);
    });

    custom_name_rows_.push_back(row);
    custom_names_panel_.Children().Append(row.container);
    refresh_custom_name_pronunciations(name_box);
    auto& added_row = custom_name_rows_.back();
    for (std::size_t index = 0;
         index < readings.size() && index < added_row.reading_choices.size(); ++index) {
        auto& choice = added_row.reading_choices[index];
        for (std::uint32_t item = 0; item < choice.Items().Size(); ++item) {
            const auto value = winrt::unbox_value<winrt::hstring>(choice.Items().GetAt(item));
            if (to_utf16(value) == readings[index]) {
                choice.SelectedIndex(static_cast<std::int32_t>(item));
                break;
            }
        }
    }
    update_custom_names_save_state();
    if (name.empty()) {
        name_box.Focus(FocusState::Programmatic);
    }
}

void SettingsWindow::remove_custom_name_row(const Button& remove_button) {
    if (!custom_names_panel_) {
        return;
    }
    for (std::size_t index = 0; index < custom_name_rows_.size(); ++index) {
        if (custom_name_rows_[index].remove_button == remove_button) {
            custom_names_panel_.Children().RemoveAt(static_cast<std::uint32_t>(index));
            custom_name_rows_.erase(custom_name_rows_.begin() +
                                    static_cast<std::ptrdiff_t>(index));
            break;
        }
    }
    update_custom_names_save_state();
}

void SettingsWindow::refresh_custom_name_pronunciations(const TextBox& name_box) {
    auto row = std::find_if(custom_name_rows_.begin(), custom_name_rows_.end(),
                            [&name_box](const CustomNameRow& candidate) {
                                return candidate.name == name_box;
                            });
    if (row == custom_name_rows_.end() || !row->pronunciations) {
        return;
    }

    row->pronunciations.Children().Clear();
    row->reading_choices.clear();
    row->missing_pronunciation = false;

    const std::u16string name = trim(to_utf16(name_box.Text()));
    if (name.empty()) {
        update_custom_names_save_state();
        return;
    }

    const auto characters = split_characters(name);
    if (!characters) {
        row->missing_pronunciation = true;
        auto warning = load_xaml_resource(IDR_CUSTOM_NAME_WARNING_XAML).as<TextBlock>();
        warning.Text(L"名字含有無效字元。");
        row->pronunciations.Children().Append(warning);
        update_custom_names_save_state();
        return;
    }

    for (const auto& [character, character_text] : *characters) {

        const auto& readings = lookup_bopomofo(character);
        if (readings.empty()) {
            row->missing_pronunciation = true;
            const std::u16string missing = u"「" + character_text + u"」查無注音";
            const auto missing_text = to_hstring(missing);
            auto warning = load_xaml_resource(IDR_CUSTOM_NAME_WARNING_XAML).as<TextBlock>();
            warning.Text(missing_text);
            row->pronunciations.Children().Append(warning);
            continue;
        }

        ComboBox choice = load_xaml_resource(IDR_CUSTOM_NAME_READING_XAML).as<ComboBox>();
        choice.Header(winrt::box_value(to_hstring(character_text)));
        for (const auto& reading : readings) {
            choice.Items().Append(winrt::box_value(to_hstring(reading)));
        }
        choice.SelectedIndex(0);
        const std::u16string accessible_name = character_text + u"的注音";
        Automation::AutomationProperties::SetName(choice, to_hstring(accessible_name));
        choice.SelectionChanged(
            [this](const auto&, const auto&) { update_custom_names_save_state(); });
        row->reading_choices.push_back(choice);
        row->pronunciations.Children().Append(choice);
    }

    update_custom_names_save_state();
}

const std::vector<std::u16string>& SettingsWindow::lookup_bopomofo(
    char32_t character) const {
    return BopomofoTable::instance().lookup(character);
}

bool SettingsWindow::collect_custom_names(std::vector<CustomNameEntry>& entries) const {
    entries.clear();
    entries.reserve(custom_name_rows_.size());
    for (const auto& row : custom_name_rows_) {
        std::u16string name = trim(to_utf16(row.name.Text()));
        if (name.empty()) continue;
        if (row.missing_pronunciation || row.reading_choices.empty()) return false;

        std::vector<std::u16string> readings;
        readings.reserve(row.reading_choices.size());
        for (const auto& choice : row.reading_choices) {
            const auto selected = choice.SelectedItem();
            if (!selected) return false;
            readings.push_back(to_utf16(winrt::unbox_value<winrt::hstring>(selected)));
        }
        entries.push_back(CustomNameEntry{
            .name = std::move(name),
            .readings = std::move(readings),
        });
    }
    return true;
}

void SettingsWindow::save_custom_names() {
    if (!save_custom_names_button_ || !custom_names_note_ ||
        !configuration_.save_custom_names_callback) {
        return;
    }

    std::vector<CustomNameEntry> entries;
    if (!collect_custom_names(entries)) {
        custom_names_note_.Text(L"部分字元沒有可用的注音。");
        custom_names_note_.Visibility(Visibility::Visible);
        return;
    }

    std::vector<std::vector<const char16_t*>> reading_pointers;
    reading_pointers.reserve(entries.size());
    std::vector<llavon_settings_custom_name> custom_names;
    custom_names.reserve(entries.size());
    for (const auto& entry : entries) {
        auto& pointers = reading_pointers.emplace_back();
        pointers.reserve(entry.readings.size());
        for (const auto& reading : entry.readings) {
            pointers.push_back(reading.c_str());
        }
        custom_names.push_back(llavon_settings_custom_name{
            .name = entry.name.c_str(),
            .readings = pointers.data(),
            .reading_count = pointers.size(),
        });
    }
    const std::int32_t result = configuration_.save_custom_names_callback(
        configuration_.save_custom_names_context, custom_names.data(), custom_names.size());
    if (result == ERROR_SUCCESS) {
        saved_custom_names_ = std::move(entries);
        save_custom_names_button_.IsEnabled(false);
        custom_names_note_.Text(L"已儲存");
    } else {
        custom_names_note_.Text(L"無法儲存自訂名字，請稍後再試。");
    }
    custom_names_note_.Visibility(Visibility::Visible);
}

void SettingsWindow::update_custom_names_save_state() {
    if (!save_custom_names_button_ || !custom_names_note_) {
        return;
    }

    std::vector<CustomNameEntry> entries;
    const bool complete = collect_custom_names(entries);
    const bool changed = entries != saved_custom_names_;
    save_custom_names_button_.IsEnabled(complete && changed);
    if (!complete) {
        custom_names_note_.Text(L"部分字元沒有可用的注音。");
        custom_names_note_.Visibility(Visibility::Visible);
    } else if (changed) {
        custom_names_note_.Text(L"尚未儲存");
        custom_names_note_.Visibility(Visibility::Visible);
    } else {
        custom_names_note_.Visibility(Visibility::Collapsed);
    }
}

void SettingsWindow::browse_model_file() {
    winrt::com_ptr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(dialog.put())))) {
        model_note_.Text(L"無法開啟檔案選擇器。");
        model_note_.Visibility(Visibility::Visible);
        return;
    }

    DWORD options = 0;
    if (SUCCEEDED(dialog->GetOptions(&options))) {
        dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST |
                           FOS_PATHMUSTEXIST);
    }
    const COMDLG_FILTERSPEC filters[] = {
        {L"GGUF 模型 (*.gguf)", L"*.gguf"},
        {L"所有檔案 (*.*)", L"*.*"},
    };
    dialog->SetFileTypes(static_cast<UINT>(std::size(filters)), filters);
    dialog->SetDefaultExtension(L"gguf");
    dialog->SetTitle(L"選擇模型檔案");

    std::filesystem::path current_path(trim(to_utf16(model_path_.Text())));
    if (!current_path.empty()) {
        if (current_path.is_relative()) {
            current_path = module_directory().parent_path() / current_path;
        }
        current_path = current_path.lexically_normal();

        winrt::com_ptr<IShellItem> current_folder;
        const auto parent = current_path.parent_path();
        if (!parent.empty() &&
            SUCCEEDED(SHCreateItemFromParsingName(
                parent.c_str(), nullptr, IID_PPV_ARGS(current_folder.put())))) {
            dialog->SetFolder(current_folder.get());
        }
        const auto filename = current_path.filename().wstring();
        if (!filename.empty()) {
            dialog->SetFileName(filename.c_str());
        }
    }

    const HRESULT shown = dialog->Show(window_);
    if (shown == HRESULT_FROM_WIN32(ERROR_CANCELLED)) return;
    if (FAILED(shown)) {
        model_note_.Text(L"無法選取模型檔案。");
        model_note_.Visibility(Visibility::Visible);
        return;
    }

    winrt::com_ptr<IShellItem> item;
    if (FAILED(dialog->GetResult(item.put()))) return;
    PWSTR selected_path = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &selected_path))) return;
    const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> owned_path(
        selected_path, CoTaskMemFree);
    model_path_.Text(owned_path.get());
}

void SettingsWindow::save_model_path() {
    if (!model_path_ || !save_model_button_ || !model_note_ ||
        !configuration_.save_model_path_callback) {
        return;
    }

    const std::u16string path = trim(to_utf16(model_path_.Text()));
    if (path.empty()) {
        model_note_.Text(L"請先選擇模型檔案。");
        model_note_.Visibility(Visibility::Visible);
        return;
    }

    const std::int32_t result = configuration_.save_model_path_callback(
        configuration_.save_model_path_context, path.c_str());
    if (result == ERROR_SUCCESS) {
        configuration_.model_path = path;
        model_path_.Text(to_hstring(path));
        save_model_button_.IsEnabled(false);
        model_note_.Text(L"已載入並儲存模型檔案位置。");
    } else {
        model_note_.Text(L"無法載入模型；請確認檔案存在且為有效的 GGUF 模型。");
    }
    model_note_.Visibility(Visibility::Visible);
}

void SettingsWindow::update_model_path_save_state() {
    if (!model_path_ || !save_model_button_ || !model_note_) return;
    const std::u16string path = trim(to_utf16(model_path_.Text()));
    const bool changed = !path.empty() && path != configuration_.model_path;
    save_model_button_.IsEnabled(changed);
    if (path.empty()) {
        model_note_.Text(L"請指定模型檔案位置。");
        model_note_.Visibility(Visibility::Visible);
    } else if (changed) {
        model_note_.Text(L"套用時會重新載入模型。");
        model_note_.Visibility(Visibility::Visible);
    } else {
        model_note_.Visibility(Visibility::Collapsed);
    }
}

void SettingsWindow::save_inference_setting() {
    if (!inference_device_ || !note_ || !configuration_.save_callback) {
        return;
    }
    const std::int32_t selected_index = inference_device_.SelectedIndex();
    if (selected_index < 0 ||
        static_cast<std::size_t>(selected_index) >= inference_options_.size()) {
        note_.Text(L"請先選擇推論裝置。");
        return;
    }

    const auto& option = inference_options_[static_cast<std::size_t>(selected_index)];
    const std::int32_t result = configuration_.save_callback(
        configuration_.save_context, option.backend, option.device_id.c_str());
    if (result == ERROR_SUCCESS) {
        configuration_.selected_backend = option.backend;
        configuration_.selected_device_id = option.device_id;
        save_inference_button_.IsEnabled(false);
        note_.Text(L"已套用並儲存");
        note_.Visibility(Visibility::Visible);
    } else {
        note_.Text(L"無法套用或儲存推論裝置設定，請稍後再試。");
        note_.Visibility(Visibility::Visible);
    }
}

void SettingsWindow::update_inference_save_state() {
    if (!inference_device_ || !save_inference_button_ || !note_) return;
    const std::int32_t selected_index = inference_device_.SelectedIndex();
    if (selected_index < 0 ||
        static_cast<std::size_t>(selected_index) >= inference_options_.size()) {
        save_inference_button_.IsEnabled(false);
        note_.Visibility(Visibility::Collapsed);
        return;
    }

    const auto& option = inference_options_[static_cast<std::size_t>(selected_index)];
    const bool changed = option.backend != configuration_.selected_backend ||
                         option.device_id != configuration_.selected_device_id;
    save_inference_button_.IsEnabled(changed);
    if (changed) {
        note_.Text(L"儲存後立即套用");
        note_.Visibility(Visibility::Visible);
    } else {
        note_.Visibility(Visibility::Collapsed);
    }
}

void SettingsWindow::begin_update_check() {
    if (!update_button_ || !update_status_ || !update_target_) {
        return;
    }

    update_button_.IsEnabled(false);
    update_status_.Text(L"正在檢查 latest 建置…");
    set_update_status_tone(UpdateStatusTone::secondary);
    if (update_download_) {
        update_download_.Visibility(Visibility::Collapsed);
    }

    const auto target = update_target_;
    if (!update_checker_.check_async([target](UpdateCheckResult result) {
            auto pending = std::make_unique<UpdateCheckResult>(std::move(result));
            std::lock_guard lock(target->mutex);
            if (target->window &&
                PostMessageW(target->window, update_result_message, 0,
                             reinterpret_cast<LPARAM>(pending.get()))) {
                pending.release();
            }
        })) {
        update_status_.Text(L"更新檢查已在進行中。");
    }
}

void SettingsWindow::apply_update_result(UpdateCheckResult result) {
    if (!update_button_ || !update_status_ || !update_download_) {
        return;
    }

    update_button_.IsEnabled(true);
    switch (result.status) {
        case UpdateCheckStatus::update_available: {
            const std::wstring status =
                L"有新版本：Build " + std::to_wstring(result.current_build) +
                L" → " + result.latest_version + L"。";
            update_status_.Text(status);
            set_update_status_tone(UpdateStatusTone::update_available);

            const std::wstring download =
                L"下載 " + result.latest_version;
            update_download_.Content(winrt::box_value(download));
            update_download_.NavigateUri(winrt::Windows::Foundation::Uri(result.release_url));
            update_download_.Visibility(Visibility::Visible);
            break;
        }
        case UpdateCheckStatus::up_to_date: {
            const std::wstring status =
                L"已是最新發布：" + result.latest_version + L"；目前安裝 " +
                build_identity(result.current_version, result.current_build, result.current_commit) +
                L"。";
            update_status_.Text(status);
            set_update_status_tone(UpdateStatusTone::success);
            update_download_.Visibility(Visibility::Collapsed);
            break;
        }
        case UpdateCheckStatus::local_newer: {
            const std::wstring status =
                L"目前 Build " + std::to_wstring(result.current_build) +
                L" 比 latest Build " + std::to_wstring(result.latest_build) + L" 新。";
            update_status_.Text(status);
            set_update_status_tone(UpdateStatusTone::information);
            update_download_.Visibility(Visibility::Collapsed);
            break;
        }
        case UpdateCheckStatus::development_build: {
            const std::wstring status =
                build_identity(result.current_version, result.current_build, result.current_commit) +
                L" 與 latest " + result.latest_version + L"（" +
                short_commit(result.latest_commit) + L"）不同，無法判斷新舊。";
            update_status_.Text(status);
            set_update_status_tone(UpdateStatusTone::secondary);
            const std::wstring download =
                L"下載 " + result.latest_version;
            update_download_.Content(winrt::box_value(download));
            update_download_.NavigateUri(winrt::Windows::Foundation::Uri(result.release_url));
            update_download_.Visibility(Visibility::Visible);
            break;
        }
        case UpdateCheckStatus::failed:
        default:
            update_status_.Text(L"無法檢查更新：" + result.error_message);
            set_update_status_tone(UpdateStatusTone::error);
            update_download_.Visibility(Visibility::Collapsed);
            break;
    }
}

void SettingsWindow::deactivate_update_target() noexcept {
    if (!update_target_) {
        return;
    }
    std::lock_guard lock(update_target_->mutex);
    update_target_->window = nullptr;
}

void SettingsWindow::discard_pending_update_results() noexcept {
    if (!window_) {
        return;
    }
    MSG message{};
    while (PeekMessageW(&message, window_, update_result_message, update_result_message, PM_REMOVE)) {
        delete reinterpret_cast<UpdateCheckResult*>(message.lParam);
    }
}

void SettingsWindow::resize_island() const noexcept {
    if (!window_ || !island_window_) {
        return;
    }
    RECT client{};
    GetClientRect(window_, &client);
    try {
        xaml_source_.SiteBridge().MoveAndResize({
            0, 0, client.right - client.left, client.bottom - client.top});
        xaml_source_.SiteBridge().Show();
    } catch (...) {
        OutputDebugStringW(L"[settings-ui] unable to resize WinUI island\n");
    }
}

void SettingsWindow::update_theme() {
    if (!window_) {
        return;
    }

    try {
        dark_theme_ = system_uses_dark_theme();
    } catch (...) {
        // Keep the last successfully detected theme if UISettings is temporarily unavailable.
    }

    const BOOL dark = dark_theme_ ? TRUE : FALSE;
    DwmSetWindowAttribute(window_, 20, &dark, sizeof(dark));

    const DWM_SYSTEMBACKDROP_TYPE backdrop = DWMSBT_MAINWINDOW;
    DwmSetWindowAttribute(
        window_, DWMWA_SYSTEMBACKDROP_TYPE, &backdrop, sizeof(backdrop));

    if (shell_) {
        shell_.RequestedTheme(dark_theme_ ? ElementTheme::Dark : ElementTheme::Light);
    }
    apply_theme_colors();
}

void SettingsWindow::apply_theme_colors() {
    set_update_status_tone(update_status_tone_);
}

void SettingsWindow::set_update_status_tone(UpdateStatusTone tone) {
    update_status_tone_ = tone;
    if (!update_status_) {
        return;
    }

    switch (tone) {
        case UpdateStatusTone::update_available:
            update_status_.Foreground(
                dark_theme_ ? solid_brush(96, 205, 255) : solid_brush(0, 95, 184));
            break;
        case UpdateStatusTone::success:
            update_status_.Foreground(
                dark_theme_ ? solid_brush(108, 203, 95) : solid_brush(48, 120, 72));
            break;
        case UpdateStatusTone::information:
            update_status_.Foreground(
                dark_theme_ ? solid_brush(117, 182, 231) : solid_brush(48, 96, 156));
            break;
        case UpdateStatusTone::error:
            update_status_.Foreground(
                dark_theme_ ? solid_brush(255, 153, 164) : solid_brush(180, 54, 54));
            break;
        case UpdateStatusTone::secondary:
        default:
            update_status_.ClearValue(TextBlock::ForegroundProperty());
            break;
    }
}

void SettingsWindow::close_xaml() noexcept {
    if (lora_dialog_timer_) {
        lora_dialog_timer_.Stop();
        lora_dialog_timer_ = nullptr;
    }
    // Detach the visual tree while both the source and its manager are alive.
    // The host closes this method again from WM_DESTROY, so keep it idempotent.
    if (xaml_source_) {
        try {
            xaml_source_.Content(nullptr);
        } catch (...) {
        }
    }
    island_window_ = nullptr;
    model_path_ = nullptr;
    browse_model_button_ = nullptr;
    save_model_button_ = nullptr;
    model_note_ = nullptr;
    active_device_status_ = nullptr;
    inference_device_ = nullptr;
    save_inference_button_ = nullptr;
    update_button_ = nullptr;
    update_status_ = nullptr;
    update_download_ = nullptr;
    note_ = nullptr;
    custom_name_rows_.clear();
    custom_names_panel_ = nullptr;
    add_custom_name_button_ = nullptr;
    save_custom_names_button_ = nullptr;
    custom_names_note_ = nullptr;
    pending_summary_ = nullptr;
    shell_ = nullptr;
    lora_dialog_open_ = false;
    close_lora_dialog_ = {};
    lora_note_ = nullptr;
    if (xaml_source_) {
        try {
            xaml_source_.Close();
        } catch (...) {
        }
        xaml_source_ = nullptr;
    }
}

}  // namespace llavon::settings
