#pragma once

#include <ctffunc.h>
#include <ctfutb.h>
#include <msctf.h>
#include <olectl.h>
#include <windows.h>
#include <winrt/base.h>

#include <functional>
#include <utility>

#include "engine/engine_types.hpp"
#include "system/globals.h"
#include "system/taskbar_theme.hpp"

namespace tsf {

enum class InputIndicator { Chinese, English, Unavailable };

class InputModeLangBarItem
    : public winrt::implements<InputModeLangBarItem, ITfLangBarItem, ITfLangBarItemButton, ITfSource> {
public:
    explicit InputModeLangBarItem(std::function<void()> on_click = {},
                                  std::function<void()> on_open_settings = {},
                                  std::function<void(POINT)> on_open_menu = {});

    HRESULT add_to_language_bar(ITfThreadMgr* thread_mgr);
    HRESULT remove_from_language_bar(ITfThreadMgr* thread_mgr);
    void set_indicator(InputIndicator indicator);
    void set_icon_color(TaskbarIconColor color);
    void refresh_theme();

    STDMETHODIMP GetInfo(TF_LANGBARITEMINFO* info) override;
    STDMETHODIMP GetStatus(DWORD* status) override;
    STDMETHODIMP Show(BOOL show) override;
    STDMETHODIMP GetTooltipString(BSTR* tooltip) override;

    STDMETHODIMP OnClick(TfLBIClick click, POINT point, const RECT* area) override;
    STDMETHODIMP InitMenu(ITfMenu* menu) override;
    STDMETHODIMP OnMenuSelect(UINT id) override;
    STDMETHODIMP GetIcon(HICON* icon) override;
    STDMETHODIMP GetText(BSTR* text) override;

    STDMETHODIMP AdviseSink(REFIID riid, IUnknown* punk, DWORD* cookie) override;
    STDMETHODIMP UnadviseSink(DWORD cookie) override;

private:
    void notify_update(DWORD flags);
    const wchar_t* mode_label() const;
    const wchar_t* tooltip_text() const;
    int icon_id() const;

    TF_LANGBARITEMINFO info_ = {};
    winrt::com_ptr<ITfLangBarItemSink> sink_;
    std::function<void()> on_click_;
    std::function<void()> on_open_settings_;
    std::function<void(POINT)> on_open_menu_;
    InputIndicator indicator_ = InputIndicator::Chinese;
    TaskbarIconColor icon_color_ = TaskbarIconColor::System;
    TaskbarTheme theme_ = current_taskbar_theme();
    DWORD status_ = 0;
    bool added_ = false;
};

}  // namespace tsf
