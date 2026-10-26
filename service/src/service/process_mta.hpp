#pragma once

#include <windows.h>
#include <combaseapi.h>

#include <winrt/base.h>

namespace llavon::service {

// Keeps the process MTA alive so worker threads that never call
// CoInitializeEx join the implicit MTA. Threads that need an STA, such as the
// XAML UI thread, still initialize their own apartment.
class ProcessMta final {
public:
    ProcessMta() { winrt::check_hresult(CoIncrementMTAUsage(&cookie_)); }
    ~ProcessMta() { CoDecrementMTAUsage(cookie_); }

    ProcessMta(const ProcessMta&) = delete;
    ProcessMta& operator=(const ProcessMta&) = delete;

private:
    CO_MTA_USAGE_COOKIE cookie_{};
};

}  // namespace llavon::service
