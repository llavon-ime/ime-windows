#pragma once

#include <msctf.h>
#include <winrt/base.h>

#include <algorithm>
#include <functional>
#include <string>
#include <vector>

#include "system/sysutil.hpp"
#include "candidateUiClient.hpp"

namespace tsf {

enum class CandidateKeyResult {
    not_handled,
    navigated,
    finalized,
    aborted,
};

// clang-format off
class CandidateListUIElement
    : public winrt::implements<
        CandidateListUIElement,
        ITfCandidateListUIElementBehavior
    >, public module_lock_updater {
    // clang-format on
public:
    // ITfUIElement
    HRESULT GetDescription(BSTR* pbstrDescription) override {
        if (!pbstrDescription) {
            return E_INVALIDARG;
        }
        *pbstrDescription = SysAllocString(L"拉風輸入法候選字");
        return (*pbstrDescription != nullptr) ? S_OK : E_OUTOFMEMORY;
    }

    HRESULT GetGUID(GUID* pguid) override {
        if (!pguid) {
            return E_INVALIDARG;
        }
        *pguid = GUID{0x62a0f624, 0x7059, 0x4f83, {0x9b, 0x2c, 0x27, 0x87, 0x10, 0x17, 0x4e, 0x65}};
        return S_OK;
    }

    HRESULT Show(BOOL bShow) override {
        shown_ = (bShow != FALSE);
        if (shown_) {
            refresh_window();
        } else {
            candidate_ui_client_.hide();
        }
        return S_OK;
    }

    HRESULT IsShown(BOOL* pbShow) override {
        if (!pbShow) {
            return E_INVALIDARG;
        }
        *pbShow = shown_ ? TRUE : FALSE;
        return S_OK;
    }

    // ITfCandidateListUIElement
    HRESULT GetUpdatedFlags(DWORD* pdwFlags) override {
        if (!pdwFlags) {
            return E_INVALIDARG;
        }
        *pdwFlags = TF_CLUIE_COUNT | TF_CLUIE_SELECTION | TF_CLUIE_STRING | TF_CLUIE_PAGEINDEX | TF_CLUIE_CURRENTPAGE;
        return S_OK;
    }

    HRESULT GetDocumentMgr(ITfDocumentMgr** ppdim) override {
        if (!ppdim) {
            return E_INVALIDARG;
        }
        *ppdim = nullptr;
        return S_OK;
    }

    HRESULT GetSelection(UINT* pnIndex) override {
        if (!pnIndex) {
            return E_INVALIDARG;
        }
        *pnIndex = selection_index;
        return S_OK;
    }

    HRESULT GetCount(UINT* pnCount) override {
        if (!pnCount) {
            return E_INVALIDARG;
        }
        *pnCount = static_cast<UINT>(candidates.size());
        return S_OK;
    }

    HRESULT GetString(UINT uIndex, BSTR* pbstr) override {
        if (!pbstr || uIndex >= candidates.size()) {
            return E_INVALIDARG;
        }
        *pbstr = SysAllocString(candidates[uIndex].c_str());
        if (*pbstr == nullptr) {
            return E_OUTOFMEMORY;
        }
        return S_OK;
    }

    HRESULT GetPageIndex(UINT* pIndex, UINT uSize, UINT* puPageCnt) override {
        if (!puPageCnt) {
            return E_INVALIDARG;
        }

        const UINT count = page_count();
        *puPageCnt = count;
        if (!pIndex) {
            return S_OK;
        }

        for (UINT i = 0; i < std::min(uSize, count); ++i) {
            pIndex[i] = i * page_size;
        }
        return S_OK;
    }

    HRESULT SetPageIndex(UINT* pIndex, UINT uPageCnt) override {
        if (!pIndex || uPageCnt == 0 || candidates.empty()) {
            return E_INVALIDARG;
        }

        const UINT requested = pIndex[0];
        if (requested >= candidates.size()) {
            return E_INVALIDARG;
        }

        const UINT old_offset = selection_index % page_size;
        current_page = requested / page_size;
        const UINT begin = current_page * page_size;
        selection_index = std::min(begin + old_offset, static_cast<UINT>(candidates.size() - 1));
        refresh_window();
        return S_OK;
    }

    HRESULT GetCurrentPage(UINT* puPage) override {
        if (!puPage) {
            return E_INVALIDARG;
        }
        *puPage = current_page;
        return S_OK;
    }

    // ITfCandidateListUIElementBehavior
    HRESULT SetSelection(UINT nIndex) override {
        if (nIndex >= candidates.size()) {
            return E_INVALIDARG;
        }
        selection_index = nIndex;
        current_page = selection_index / page_size;
        refresh_window();
        return S_OK;
    }

    HRESULT Finalize() override {
        if (finalize_callback && !candidates.empty() && selection_index < candidates.size()) {
            finalize_callback(candidates[selection_index]);
        }
        candidate_ui_client_.hide();
        return S_OK;
    }

    HRESULT Abort() override {
        candidate_ui_client_.hide();
        return S_OK;
    }

public:
    void set_anchor_rect(const RECT& rect) {
        anchor_rect = rect;
        has_anchor_rect = true;
    }

    void clear_anchor_rect() {
        has_anchor_rect = false;
    }

    void set_owner_window(HWND window) noexcept {
        owner_window = window;
    }

    void disconnect() noexcept {
        candidate_ui_client_.disconnect();
    }

    bool is_shown() const {
        return shown_;
    }

    void update(const std::vector<std::wstring>& can, std::function<void(std::wstring)> callback) {
        candidates = can;
        selection_index = 0;
        current_page = 0;
        expanded = false;
        finalize_callback = std::move(callback);
        refresh_window();
    }

    bool is_expanded() const {
        return expanded;
    }

    bool expand() {
        if (expanded) {
            return false;
        }
        expanded = true;
        refresh_window();
        return true;
    }

    bool collapse() {
        if (!expanded) {
            return false;
        }
        expanded = false;
        refresh_window();
        return true;
    }

    bool page_prev() {
        if (candidates.empty() || current_page == 0) {
            return false;
        }

        const UINT offset = selection_index % page_size;
        --current_page;
        const UINT begin = current_page * page_size;
        selection_index = std::min(begin + offset, static_cast<UINT>(candidates.size() - 1));
        refresh_window();
        return true;
    }

    bool page_next() {
        const UINT pages = page_count();
        if (candidates.empty() || current_page + 1 >= pages) {
            return false;
        }

        const UINT offset = selection_index % page_size;
        ++current_page;
        const UINT begin = current_page * page_size;
        selection_index = std::min(begin + offset, static_cast<UINT>(candidates.size() - 1));
        refresh_window();
        return true;
    }

    bool select_prev_in_page() {
        if (candidates.empty()) {
            return false;
        }
        const UINT begin = current_page * page_size;
        const UINT end = page_end_exclusive(current_page);
        if (selection_index <= begin) {
            selection_index = end - 1;
        } else {
            --selection_index;
        }
        refresh_window();
        return true;
    }

    bool select_next_in_page() {
        if (candidates.empty()) {
            return false;
        }
        const UINT begin = current_page * page_size;
        const UINT end = page_end_exclusive(current_page);
        if (selection_index + 1 >= end) {
            selection_index = begin;
        } else {
            ++selection_index;
        }
        refresh_window();
        return true;
    }

    bool can_handle_key(WPARAM wParam) const {
        return wParam == VK_UP || wParam == VK_DOWN || wParam == VK_LEFT || wParam == VK_RIGHT || wParam == VK_RETURN ||
               wParam == VK_SPACE || wParam == VK_ESCAPE || (wParam >= '1' && wParam <= '9') ||
               (wParam >= VK_NUMPAD1 && wParam <= VK_NUMPAD9);
    }

    CandidateKeyResult handle_key(WPARAM wParam) {
        if (!can_handle_key(wParam) || candidates.empty()) {
            return CandidateKeyResult::not_handled;
        }

        if (wParam == VK_UP) {
            select_prev_in_page();
            return CandidateKeyResult::navigated;
        }
        if (wParam == VK_DOWN) {
            select_next_in_page();
            return CandidateKeyResult::navigated;
        }
        if (wParam == VK_LEFT) {
            page_prev();
            return CandidateKeyResult::navigated;
        }
        if (wParam == VK_RIGHT) {
            if (!is_expanded()) {
                expand();
            } else {
                page_next();
            }
            return CandidateKeyResult::navigated;
        }
        if (wParam == VK_RETURN || wParam == VK_SPACE) {
            Finalize();
            return CandidateKeyResult::finalized;
        }
        if (wParam == VK_ESCAPE) {
            Abort();
            return CandidateKeyResult::aborted;
        }

        const UINT page_begin = current_page * page_size;
        if (wParam >= '1' && wParam <= '9') {
            const UINT index = page_begin + static_cast<UINT>(wParam - '1');
            if (index < candidates.size()) {
                SetSelection(index);
                Finalize();
                return CandidateKeyResult::finalized;
            }
            return CandidateKeyResult::not_handled;
        }
        if (wParam >= VK_NUMPAD1 && wParam <= VK_NUMPAD9) {
            const UINT index = page_begin + static_cast<UINT>(wParam - VK_NUMPAD1);
            if (index < candidates.size()) {
                SetSelection(index);
                Finalize();
                return CandidateKeyResult::finalized;
            }
        }

        return CandidateKeyResult::not_handled;
    }

private:
    inline static constexpr uint32_t page_size = 9;
    inline static constexpr uint32_t expanded_columns = 4;
    inline static constexpr uint32_t preferred_current_column = 1;
    std::vector<std::wstring> candidates;
    UINT selection_index = 0;
    UINT current_page = 0;
    bool shown_ = false;
    bool expanded = false;
    bool has_anchor_rect = false;
    RECT anchor_rect = {};
    HWND owner_window = nullptr;
    std::function<void(std::wstring)> finalize_callback;
    CandidateUiClient candidate_ui_client_;

private:
    UINT page_count() const {
        if (candidates.empty()) {
            return 0;
        }
        return static_cast<UINT>((candidates.size() + page_size - 1) / page_size);
    }

    UINT page_end_exclusive(UINT page) const {
        const UINT begin = page * page_size;
        return std::min(begin + page_size, static_cast<UINT>(candidates.size()));
    }

    void refresh_window() {
        if (candidates.empty()) {
            candidate_ui_client_.hide();
            return;
        }

        current_page = std::min(current_page, page_count() - 1);
        selection_index = std::min(selection_index, static_cast<UINT>(candidates.size() - 1));

        UINT start_page = current_page;
        UINT visible_pages = 1;
        if (expanded) {
            const UINT pages = page_count();
            visible_pages = std::min(expanded_columns, pages);
            if (pages > visible_pages) {
                if (current_page <= preferred_current_column) {
                    start_page = 0;
                } else {
                    start_page = current_page - preferred_current_column;
                }
                const UINT max_start = pages - visible_pages;
                start_page = std::min(start_page, max_start);
            } else {
                start_page = 0;
            }
        }

        const UINT begin = start_page * page_size;
        const UINT end = std::min((start_page + visible_pages) * page_size, static_cast<UINT>(candidates.size()));

        std::vector<std::wstring> page_items;
        page_items.reserve(end - begin);
        for (UINT i = begin; i < end; ++i) {
            page_items.push_back(candidates[i]);
        }

        const UINT current_begin = current_page * page_size;
        const UINT current_end = page_end_exclusive(current_page);
        UINT offset = 0;
        if (selection_index >= current_begin && current_end > current_begin) {
            offset = std::min(selection_index - current_begin, current_end - current_begin - 1);
        }

        const UINT pages = page_count();
        const UINT number_column = current_page - start_page;
        const UINT local_selection = number_column * page_size + offset;

        if (!shown_) {
            return;
        }

        RECT presentation_anchor = anchor_rect;
        if (!has_anchor_rect) {
            POINT cursor = {};
            if (GetCursorPos(&cursor) == FALSE) {
                return;
            }
            presentation_anchor = RECT{cursor.x, cursor.y, cursor.x, cursor.y};
        }

        CandidateUiPresentation presentation;
        presentation.owner_window = static_cast<uint64_t>(reinterpret_cast<ULONG_PTR>(owner_window));
        presentation.anchor_x = presentation_anchor.left;
        presentation.anchor_y = presentation_anchor.bottom;
        presentation.anchor_top = presentation_anchor.top;
        presentation.candidates = std::move(page_items);
        presentation.selection_index = local_selection;
        presentation.layout_columns = visible_pages;
        presentation.number_column = number_column;
        presentation.can_prev_page = current_page > 0;
        presentation.can_next_page = current_page + 1 < pages;
        candidate_ui_client_.present(presentation);
    }
};

}  // namespace tsf
