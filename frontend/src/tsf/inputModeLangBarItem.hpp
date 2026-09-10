#pragma once

#include <ctffunc.h>
#include <ctfutb.h>
#include <msctf.h>
#include <olectl.h>
#include <windows.h>
#include <winrt/base.h>

#include <functional>
#include <utility>

#include "engine/impl/engine.h"
#include "system/globals.h"
#include "system/sysutil.hpp"

namespace tsf {

class InputModeLangBarItem
    : public winrt::implements<InputModeLangBarItem, ITfLangBarItem, ITfLangBarItemButton, ITfSource>,
      public module_lock_updater {
public:
    explicit InputModeLangBarItem(std::function<void()> on_click = {});

    HRESULT add_to_language_bar(ITfThreadMgr* thread_mgr);
    HRESULT remove_from_language_bar(ITfThreadMgr* thread_mgr);
    void set_mode(InputMode mode);

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

    TF_LANGBARITEMINFO info_ = {};
    winrt::com_ptr<ITfLangBarItemSink> sink_;
    std::function<void()> on_click_;
    InputMode mode_ = InputMode::Chinese;
    DWORD status_ = 0;
    bool added_ = false;
};

}  // namespace tsf
