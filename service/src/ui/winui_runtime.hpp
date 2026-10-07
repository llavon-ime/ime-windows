#pragma once

#include <memory>

namespace llavon::ui {

// Lives on a UI STA, outside all windows and XAML objects on that STA.
class WinuiRuntime final {
public:
    WinuiRuntime();
    ~WinuiRuntime();
    WinuiRuntime(const WinuiRuntime&) = delete;
    WinuiRuntime& operator=(const WinuiRuntime&) = delete;

private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace llavon::ui
