#pragma once

#include <windows.h>

#include <string>

namespace llavon::candidate {

class DebugSink final {
public:
    static DebugSink& instance() noexcept {
        static DebugSink sink;
        return sink;
    }

    void send(const wchar_t* category, const std::wstring& message) const noexcept {
        std::wstring line = L"[candidate-ui][";
        line += category ? category : L"LOG";
        line += L"] ";
        line += message;
        line += L"\n";
        OutputDebugStringW(line.c_str());
    }
};

}  // namespace llavon::candidate
