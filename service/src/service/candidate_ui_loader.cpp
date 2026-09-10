#include "candidate_ui_loader.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace llavon::service {
namespace {

constexpr wchar_t candidate_ui_filename[] = L"llavon-ime-candidate-ui.dll";

std::filesystem::path executable_directory() {
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD copied =
            GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (copied == 0) {
            return {};
        }
        if (copied < buffer.size() - 1) {
            buffer.resize(copied);
            return std::filesystem::path(buffer).parent_path();
        }
        buffer.resize(buffer.size() * 2);
    }
}

template <typename Function>
Function resolve(HMODULE module, const char* name) {
    return reinterpret_cast<Function>(GetProcAddress(module, name));
}

}  // namespace

CandidateUiLoader::~CandidateUiLoader() {
    std::lock_guard lock(mutex_);
    if (!module_) {
        return;
    }

    bool can_unload = true;
    if (started_ && stop_) {
        can_unload = stop_() == 0;
    }
    if (can_unload) {
        FreeLibrary(module_);
    }
}

bool CandidateUiLoader::present(const CandidateUiSnapshot& snapshot) {
    std::lock_guard lock(mutex_);
    if (!start()) {
        return false;
    }

    std::vector<llavon_candidate_ui_string_view> candidates;
    candidates.reserve(snapshot.candidates.size());
    for (const auto& value : snapshot.candidates) {
        candidates.push_back(
            {value.data(), static_cast<uint32_t>(value.size())});
    }

    const llavon_candidate_ui_presentation presentation{
        sizeof(llavon_candidate_ui_presentation),
        snapshot.owner_window,
        snapshot.anchor_x,
        snapshot.anchor_y,
        static_cast<uint32_t>(candidates.size()),
        candidates.data(),
        snapshot.selection_index,
        snapshot.layout_columns,
        snapshot.number_column,
        static_cast<uint8_t>(snapshot.can_prev_page ? 1 : 0),
        static_cast<uint8_t>(snapshot.can_next_page ? 1 : 0),
        0,
        snapshot.anchor_top,
    };
    const std::int32_t result = present_(&presentation);
    if (result != 0) {
        report_error(L"The candidate UI snapshot was rejected.", result);
        return false;
    }
    return true;
}

void CandidateUiLoader::hide() noexcept {
    std::lock_guard lock(mutex_);
    if (started_ && hide_) {
        hide_();
    }
}

bool CandidateUiLoader::start() {
    if (!load()) {
        report_error(L"The candidate UI module could not be loaded.", static_cast<std::int32_t>(GetLastError()));
        return false;
    }
    if (started_) {
        return true;
    }

    const std::int32_t result = start_();
    if (result != 0) {
        report_error(L"The candidate UI thread could not be started.", result);
        return false;
    }
    started_ = true;
    return true;
}

bool CandidateUiLoader::load() {
    if (module_) {
        return true;
    }

    const std::filesystem::path directory = executable_directory();
    if (directory.empty()) {
        return false;
    }
    const std::filesystem::path module_path = directory / candidate_ui_filename;
    module_ = LoadLibraryExW(
        module_path.c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!module_) {
        return false;
    }

    start_ = resolve<StartFunction>(module_, "llavon_candidate_ui_start");
    present_ = resolve<PresentFunction>(module_, "llavon_candidate_ui_present");
    hide_ = resolve<HideFunction>(module_, "llavon_candidate_ui_hide");
    stop_ = resolve<StopFunction>(module_, "llavon_candidate_ui_stop");
    if (!start_ || !present_ || !hide_ || !stop_) {
        FreeLibrary(module_);
        module_ = nullptr;
        start_ = nullptr;
        present_ = nullptr;
        hide_ = nullptr;
        stop_ = nullptr;
        return false;
    }
    return true;
}

void CandidateUiLoader::report_error(const wchar_t* detail, std::int32_t error) const noexcept {
    std::wstring message = L"[candidate-ui] ";
    message += detail;
    if (error != 0) {
        message += L" error=";
        message += std::to_wstring(error);
    }
    message += L"\n";
    OutputDebugStringW(message.c_str());
}

}  // namespace llavon::service
