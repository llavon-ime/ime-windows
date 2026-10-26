#pragma once

#include <msctf.h>
#include <winrt/base.h>

#include <functional>

namespace tsf {

class EditSession : public winrt::implements<EditSession, ITfEditSession> {
public:
    STDMETHODIMP DoEditSession(TfEditCookie ec) override try {
        if (oper) {
            oper(ec);
        }
        return S_OK;
    } catch (...) {
        return winrt::to_hresult();
    }

    void set_operation(std::function<void(TfEditCookie)> func) {
        oper = std::move(func);
    }

private:
    std::function<void(TfEditCookie)> oper;
};

}  // namespace tsf
