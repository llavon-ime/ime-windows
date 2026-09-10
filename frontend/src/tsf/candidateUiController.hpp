#pragma once

#include <msctf.h>
#include <winrt/base.h>

#include <functional>
#include <string>
#include <vector>

#include "candidateListUI.hpp"

namespace tsf {

class CandidateUiController {
public:
    void attach(ITfThreadMgr* thread_mgr, TfClientId client_id);
    void detach();

    bool is_active() const;
    bool can_handle_key(WPARAM wParam) const;
    CandidateKeyResult handle_key(WPARAM wParam);

    void show(ITfContext* context, const std::vector<std::wstring>& candidates,
              std::function<void(std::wstring)> on_finalize);
    void expand();
    void hide();

private:
    winrt::com_ptr<ITfUIElementMgr> get_ui_element_mgr() const;
    bool query_owner_window(ITfContext* context, HWND* owner_window) const;
    bool query_anchor_rect(ITfContext* context, RECT* anchor_rect) const;
    void dismiss_ui_element();

    winrt::com_ptr<ITfThreadMgr> thread_mgr_;
    TfClientId client_id_ = TF_CLIENTID_NULL;
    winrt::com_ptr<CandidateListUIElement> element_ = winrt::make_self<CandidateListUIElement>();
    DWORD ui_element_id_ = TF_INVALID_COOKIE;
};

}  // namespace tsf
