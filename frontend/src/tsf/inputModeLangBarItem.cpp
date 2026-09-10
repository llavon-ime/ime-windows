#include "inputModeLangBarItem.hpp"

#include <strsafe.h>

#include "resource.h"

namespace tsf {

namespace {

constexpr DWORD kSinkCookie = 1;

}  // namespace

InputModeLangBarItem::InputModeLangBarItem(std::function<void()> on_click) : on_click_(std::move(on_click)) {
    info_.clsidService = Globals::text_service_clsid;
    info_.guidItem = GUID_LBI_INPUTMODE;
    info_.dwStyle = TF_LBI_STYLE_BTN_BUTTON | TF_LBI_STYLE_HIDDENSTATUSCONTROL | TF_LBI_STYLE_SHOWNINTRAY;
    info_.ulSort = 0;
    StringCchCopyW(info_.szDescription, ARRAYSIZE(info_.szDescription), L"Input mode");
}

HRESULT InputModeLangBarItem::add_to_language_bar(ITfThreadMgr* thread_mgr) {
    if (!thread_mgr) {
        return E_INVALIDARG;
    }
    if (added_) {
        return S_OK;
    }

    winrt::com_ptr<ITfLangBarItemMgr> manager;
    HRESULT hr = thread_mgr->QueryInterface(IID_PPV_ARGS(manager.put()));
    if (FAILED(hr)) {
        return hr;
    }

    ITfLangBarItem* raw_item = nullptr;
    hr = QueryInterface(IID_ITfLangBarItem, reinterpret_cast<void**>(&raw_item));
    if (FAILED(hr)) {
        return hr;
    }

    winrt::com_ptr<ITfLangBarItem> item;
    item.attach(raw_item);
    hr = manager->AddItem(item.get());
    if (SUCCEEDED(hr)) {
        added_ = true;
    }
    return hr;
}

HRESULT InputModeLangBarItem::remove_from_language_bar(ITfThreadMgr* thread_mgr) {
    if (!thread_mgr) {
        return E_INVALIDARG;
    }
    if (!added_) {
        return S_OK;
    }

    winrt::com_ptr<ITfLangBarItemMgr> manager;
    HRESULT hr = thread_mgr->QueryInterface(IID_PPV_ARGS(manager.put()));
    if (FAILED(hr)) {
        return hr;
    }

    ITfLangBarItem* raw_item = nullptr;
    hr = QueryInterface(IID_ITfLangBarItem, reinterpret_cast<void**>(&raw_item));
    if (FAILED(hr)) {
        return hr;
    }

    winrt::com_ptr<ITfLangBarItem> item;
    item.attach(raw_item);
    hr = manager->RemoveItem(item.get());
    if (SUCCEEDED(hr)) {
        added_ = false;
    }
    return hr;
}

void InputModeLangBarItem::set_mode(InputMode mode) {
    if (mode_ == mode) {
        return;
    }

    mode_ = mode;
    notify_update(TF_LBI_ICON | TF_LBI_TEXT | TF_LBI_TOOLTIP);
}

STDMETHODIMP InputModeLangBarItem::GetInfo(TF_LANGBARITEMINFO* info) {
    if (!info) {
        return E_INVALIDARG;
    }

    *info = info_;
    return S_OK;
}

STDMETHODIMP InputModeLangBarItem::GetStatus(DWORD* status) {
    if (!status) {
        return E_INVALIDARG;
    }

    *status = status_;
    return S_OK;
}

STDMETHODIMP InputModeLangBarItem::Show(BOOL show) {
    const DWORD old_status = status_;
    if (show) {
        status_ &= ~TF_LBI_STATUS_HIDDEN;
    } else {
        status_ |= TF_LBI_STATUS_HIDDEN;
    }

    if (old_status != status_) {
        notify_update(TF_LBI_STATUS);
    }
    return S_OK;
}

STDMETHODIMP InputModeLangBarItem::GetTooltipString(BSTR* tooltip) {
    if (!tooltip) {
        return E_INVALIDARG;
    }

    *tooltip = SysAllocString(tooltip_text());
    return *tooltip ? S_OK : E_OUTOFMEMORY;
}

STDMETHODIMP InputModeLangBarItem::OnClick(TfLBIClick /*click*/, POINT /*point*/, const RECT* /*area*/) {
    if (on_click_) {
        on_click_();
    }
    return S_OK;
}

STDMETHODIMP InputModeLangBarItem::InitMenu(ITfMenu* /*menu*/) {
    return S_OK;
}

STDMETHODIMP InputModeLangBarItem::OnMenuSelect(UINT /*id*/) {
    return S_OK;
}

STDMETHODIMP InputModeLangBarItem::GetIcon(HICON* icon) {
    if (!icon) {
        return E_INVALIDARG;
    }

    const int icon_id = mode_ == InputMode::Chinese ? IDI_INPUT_MODE_CHINESE : IDI_INPUT_MODE_ENGLISH;
    const int icon_width = GetSystemMetrics(SM_CXSMICON);
    const int icon_height = GetSystemMetrics(SM_CYSMICON);
    *icon = reinterpret_cast<HICON>(LoadImageW(Globals::hinstance, MAKEINTRESOURCEW(icon_id), IMAGE_ICON, icon_width,
                                               icon_height, LR_DEFAULTCOLOR));
    return *icon ? S_OK : E_FAIL;
}

STDMETHODIMP InputModeLangBarItem::GetText(BSTR* text) {
    if (!text) {
        return E_INVALIDARG;
    }

    *text = SysAllocString(mode_label());
    return *text ? S_OK : E_OUTOFMEMORY;
}

STDMETHODIMP InputModeLangBarItem::AdviseSink(REFIID riid, IUnknown* punk, DWORD* cookie) {
    if (!cookie) {
        return E_INVALIDARG;
    }
    *cookie = 0;

    if (!IsEqualIID(riid, IID_ITfLangBarItemSink)) {
        return CONNECT_E_CANNOTCONNECT;
    }
    if (!punk) {
        return E_INVALIDARG;
    }
    if (sink_) {
        return CONNECT_E_ADVISELIMIT;
    }

    ITfLangBarItemSink* raw_sink = nullptr;
    HRESULT hr = punk->QueryInterface(IID_ITfLangBarItemSink, reinterpret_cast<void**>(&raw_sink));
    if (FAILED(hr)) {
        return hr;
    }

    sink_.attach(raw_sink);
    *cookie = kSinkCookie;
    return S_OK;
}

STDMETHODIMP InputModeLangBarItem::UnadviseSink(DWORD cookie) {
    if (cookie != kSinkCookie || !sink_) {
        return CONNECT_E_NOCONNECTION;
    }

    sink_ = nullptr;
    return S_OK;
}

void InputModeLangBarItem::notify_update(DWORD flags) {
    if (sink_) {
        sink_->OnUpdate(flags);
    }
}

const wchar_t* InputModeLangBarItem::mode_label() const {
    return mode_ == InputMode::Chinese ? L"\x4E2D" : L"\x82F1";
}

const wchar_t* InputModeLangBarItem::tooltip_text() const {
    return mode_ == InputMode::Chinese ? L"Chinese input mode" : L"English input mode";
}

}  // namespace tsf
