#pragma once

#include <functional>
#include <memory>

namespace llavon::ui {

// Owns the process's WinUI Application and STA. Construct before UI clients and
// destroy after them; WinUI cannot be restarted after terminal shutdown.
class UiThread final {
public:
    UiThread();
    ~UiThread();
    UiThread(const UiThread&) = delete;
    UiThread& operator=(const UiThread&) = delete;

    // Executes on the STA and propagates exceptions to the caller.
    void invoke(std::function<void()> work) const;

private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace llavon::ui
