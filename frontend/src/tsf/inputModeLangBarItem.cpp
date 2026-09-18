#include "inputModeLangBarItem.hpp"

#include <strsafe.h>

#include "resource.h"

namespace tsf {

namespace {

constexpr DWORD kSinkCookie = 1;
constexpr UINT kSettingsMenuId = 1;
constexpr wchar_t kSettingsMenuLabel[] = L"設定";
constexpr wchar_t kSettingsGlyph[] = L"\xE713";
constexpr int kMenuIconSize = 16;

struct MenuIconBitmaps {
    HBITMAP bitmap = nullptr;
    HBITMAP mask = nullptr;
};

HFONT create_symbol_font(HDC dc, const wchar_t* face_name) {
    HFONT font = CreateFontW(-kMenuIconSize, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             NONANTIALIASED_QUALITY, DEFAULT_PITCH, face_name);
    if (!font) {
        return nullptr;
    }

    const HGDIOBJ previous_font = SelectObject(dc, font);
    wchar_t actual_face[LF_FACESIZE] = {};
    const int face_length = GetTextFaceW(dc, ARRAYSIZE(actual_face), actual_face);
    SelectObject(dc, previous_font);
    if (face_length == 0 ||
        CompareStringOrdinal(actual_face, -1, face_name, -1, TRUE) != CSTR_EQUAL) {
        DeleteObject(font);
        return nullptr;
    }
    return font;
}

MenuIconBitmaps create_settings_menu_icon() {
    MenuIconBitmaps result;
    result.bitmap = CreateBitmap(kMenuIconSize, kMenuIconSize, 1, 1, nullptr);
    result.mask = CreateBitmap(kMenuIconSize, kMenuIconSize, 1, 1, nullptr);
    if (!result.bitmap || !result.mask) {
        if (result.bitmap) DeleteObject(result.bitmap);
        if (result.mask) DeleteObject(result.mask);
        return {};
    }

    const HDC dc = CreateCompatibleDC(nullptr);
    if (!dc) {
        DeleteObject(result.bitmap);
        DeleteObject(result.mask);
        return {};
    }

    HFONT font = create_symbol_font(dc, L"Segoe Fluent Icons");
    if (!font) {
        font = create_symbol_font(dc, L"Segoe MDL2 Assets");
    }
    if (!font) {
        DeleteDC(dc);
        DeleteObject(result.bitmap);
        DeleteObject(result.mask);
        return {};
    }

    const HGDIOBJ previous_bitmap = SelectObject(dc, result.bitmap);
    const HGDIOBJ previous_font = SelectObject(dc, font);
    PatBlt(dc, 0, 0, kMenuIconSize, kMenuIconSize, WHITENESS);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(0, 0, 0));
    RECT bounds{0, 0, kMenuIconSize, kMenuIconSize};
    DrawTextW(dc, kSettingsGlyph, 1, &bounds,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    SelectObject(dc, result.mask);
    PatBlt(dc, 0, 0, kMenuIconSize, kMenuIconSize, BLACKNESS);

    SelectObject(dc, previous_font);
    SelectObject(dc, previous_bitmap);
    DeleteObject(font);
    DeleteDC(dc);
    return result;
}

}  // namespace

InputModeLangBarItem::InputModeLangBarItem(std::function<void()> on_click,
                                           std::function<void()> on_open_settings)
    : on_click_(std::move(on_click)),
      on_open_settings_(std::move(on_open_settings)) {
    info_.clsidService = Globals::text_service_clsid;
    info_.guidItem = GUID_LBI_INPUTMODE;
    info_.dwStyle = TF_LBI_STYLE_BTN_BUTTON | TF_LBI_STYLE_BTN_MENU |
                    TF_LBI_STYLE_HIDDENSTATUSCONTROL | TF_LBI_STYLE_SHOWNINTRAY;
    info_.ulSort = 0;
    StringCchCopyW(info_.szDescription, ARRAYSIZE(info_.szDescription), L"Input mode");

    const MenuIconBitmaps settings_icon = create_settings_menu_icon();
    settings_menu_bitmap_ = settings_icon.bitmap;
    settings_menu_mask_ = settings_icon.mask;
}

InputModeLangBarItem::~InputModeLangBarItem() {
    if (settings_menu_bitmap_) {
        DeleteObject(settings_menu_bitmap_);
    }
    if (settings_menu_mask_) {
        DeleteObject(settings_menu_mask_);
    }
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

STDMETHODIMP InputModeLangBarItem::OnClick(TfLBIClick click, POINT /*point*/, const RECT* /*area*/) {
    if (click == TF_LBI_CLK_LEFT && on_click_) {
        on_click_();
    }
    return S_OK;
}

STDMETHODIMP InputModeLangBarItem::InitMenu(ITfMenu* menu) {
    if (!menu) {
        return E_INVALIDARG;
    }

    return menu->AddMenuItem(kSettingsMenuId, 0, settings_menu_bitmap_, settings_menu_mask_,
                             kSettingsMenuLabel, ARRAYSIZE(kSettingsMenuLabel) - 1, nullptr);
}

STDMETHODIMP InputModeLangBarItem::OnMenuSelect(UINT id) {
    if (id != kSettingsMenuId) {
        return E_INVALIDARG;
    }
    if (on_open_settings_) {
        on_open_settings_();
    }
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
