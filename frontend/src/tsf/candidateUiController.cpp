#include "candidateUiController.hpp"

#include "editSession.hpp"

namespace tsf {

void CandidateUiController::attach(ITfThreadMgr* thread_mgr, TfClientId client_id) {
    thread_mgr_.copy_from(thread_mgr);
    client_id_ = client_id;
}

void CandidateUiController::detach() {
    hide();
    element_->disconnect();
    thread_mgr_ = nullptr;
    client_id_ = TF_CLIENTID_NULL;
}

bool CandidateUiController::is_active() const {
    return element_ && element_->is_shown();
}

bool CandidateUiController::can_handle_key(WPARAM wParam) const {
    return is_active() && element_->can_handle_key(wParam);
}

CandidateKeyResult CandidateUiController::handle_key(WPARAM wParam) {
    if (!is_active()) {
        return CandidateKeyResult::not_handled;
    }

    const CandidateKeyResult result = element_->handle_key(wParam);
    if (result == CandidateKeyResult::aborted || result == CandidateKeyResult::not_handled) {
        hide();
    }
    return result;
}

void CandidateUiController::show(ITfContext* context, const std::vector<std::wstring>& candidates,
                                 std::function<void(std::wstring)> on_finalize) {
    if (!context || candidates.empty()) {
        hide();
        return;
    }

    winrt::com_ptr<ITfUIElementMgr> ui_element_mgr = get_ui_element_mgr();
    if (!ui_element_mgr) {
        return;
    }

    HWND owner_window = nullptr;
    query_owner_window(context, &owner_window);
    element_->set_owner_window(owner_window);

    RECT anchor_rect = {};
    if (query_anchor_rect(context, &anchor_rect)) {
        element_->set_anchor_rect(anchor_rect);
    } else {
        element_->clear_anchor_rect();
    }

    element_->update(candidates, [this, callback = std::move(on_finalize)](std::wstring word) mutable {
        if (callback) {
            callback(std::move(word));
        }
        dismiss_ui_element();
    });

    if (is_active()) {
        ui_element_mgr->UpdateUIElement(ui_element_id_);
        element_->Show(TRUE);
        return;
    }

    BOOL should_show = TRUE;
    const HRESULT hr = ui_element_mgr->BeginUIElement(element_.get(), &should_show, &ui_element_id_);
    if (FAILED(hr)) {
        ui_element_id_ = TF_INVALID_COOKIE;
        return;
    }

    if (should_show) {
        element_->Show(TRUE);
    }
}

void CandidateUiController::expand() {
    if (element_) {
        element_->expand();
    }
}

void CandidateUiController::hide() {
    dismiss_ui_element();
}

winrt::com_ptr<ITfUIElementMgr> CandidateUiController::get_ui_element_mgr() const {
    if (!thread_mgr_) {
        return nullptr;
    }

    winrt::com_ptr<ITfUIElementMgr> ui_element_mgr;
    if (FAILED(thread_mgr_->QueryInterface<ITfUIElementMgr>(ui_element_mgr.put()))) {
        return nullptr;
    }
    return ui_element_mgr;
}

bool CandidateUiController::query_owner_window(ITfContext* context, HWND* owner_window) const {
    if (!context || !owner_window) {
        return false;
    }

    *owner_window = nullptr;

    winrt::com_ptr<ITfContextView> context_view;
    const HRESULT hr_view = context->GetActiveView(context_view.put());
    if (SUCCEEDED(hr_view) && context_view) {
        HWND context_window = nullptr;
        if (SUCCEEDED(context_view->GetWnd(&context_window)) && context_window != nullptr) {
            *owner_window = context_window;
            return true;
        }
    }

    HWND focus_window = GetFocus();
    if (focus_window == nullptr) {
        return false;
    }

    *owner_window = focus_window;
    return true;
}

bool CandidateUiController::query_anchor_rect(ITfContext* context, RECT* anchor_rect) const {
    if (!context || !anchor_rect || client_id_ == TF_CLIENTID_NULL) {
        return false;
    }

    winrt::com_ptr<ITfContextView> context_view;
    HRESULT hr = context->GetActiveView(context_view.put());
    if (FAILED(hr) || !context_view) {
        return false;
    }

    RECT text_rect = {};
    bool found = false;
    winrt::com_ptr<EditSession> edit_session = winrt::make_self<EditSession>();
    edit_session->set_operation([context, context_view, &text_rect, &found](TfEditCookie ec) {
        TF_SELECTION selection = {};
        ULONG fetched = 0;
        const HRESULT hr_selection = context->GetSelection(ec, TF_DEFAULT_SELECTION, 1, &selection, &fetched);
        if (FAILED(hr_selection) || fetched == 0 || !selection.range) {
            return;
        }

        winrt::com_ptr<ITfRange> range;
        range.attach(selection.range);

        RECT rc = {};
        BOOL clipped = FALSE;
        const HRESULT hr_text_ext = context_view->GetTextExt(ec, range.get(), &rc, &clipped);
        if (FAILED(hr_text_ext)) {
            return;
        }

        text_rect = rc;
        found = true;
    });

    HRESULT hr_session = E_FAIL;
    hr = context->RequestEditSession(client_id_, edit_session.get(), TF_ES_READ | TF_ES_SYNC, &hr_session);
    if (FAILED(hr) || FAILED(hr_session) || !found) {
        return false;
    }

    *anchor_rect = text_rect;
    return true;
}

void CandidateUiController::dismiss_ui_element() {
    if (!element_) {
        return;
    }

    element_->Show(FALSE);
    element_->clear_anchor_rect();

    if (ui_element_id_ == TF_INVALID_COOKIE) {
        return;
    }

    winrt::com_ptr<ITfUIElementMgr> ui_element_mgr = get_ui_element_mgr();
    if (ui_element_mgr) {
        ui_element_mgr->EndUIElement(ui_element_id_);
    }
    ui_element_id_ = TF_INVALID_COOKIE;
}

}  // namespace tsf
