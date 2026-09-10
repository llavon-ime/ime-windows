#pragma once

#include <msctf.h>
#include <winrt/base.h>

#include <functional>

namespace tsf {

class EditSession : public winrt::implements<EditSession, ITfEditSession> {
public:
    STDMETHODIMP DoEditSession(TfEditCookie ec) override {
        if (oper) {
            oper(ec);
        }
        return S_OK;
    }

    void set_operation(std::function<void(TfEditCookie)> func) {
        oper = std::move(func);
    }

private:
    std::function<void(TfEditCookie)> oper;
};

}  // namespace tsf
