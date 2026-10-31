#pragma once

#include <winrt/Windows.Foundation.h>

#include <cstdint>
#include <string>
#include <vector>

#include "core/bopomofo.hpp"
#include "engine/engine_types.hpp"

namespace tsf {

// Driven by the engine worker's single pump coroutine, one request at a time.
// The backend is a local named pipe: while its process lives every request is
// answered, and when it dies the pipe breaks. There are no timeouts or retries.
class IEngine {
public:
    using IAsyncAction = winrt::Windows::Foundation::IAsyncAction;
    template <typename T>
    using IAsyncOperation = winrt::Windows::Foundation::IAsyncOperation<T>;

    virtual ~IEngine() = default;
    virtual IAsyncAction ready() = 0;
    virtual IAsyncAction prefill(std::u16string context) = 0;
    // Returns false only when the backend is gone; padding is updated in place.
    virtual IAsyncOperation<bool> predict(std::u16string context, std::vector<BopomofoPos>& padding) = 0;
    virtual IAsyncOperation<std::uint64_t> record_commit(CommitSample sample) = 0;
    virtual IAsyncAction discard_commit(std::uint64_t token) = 0;
    // InputMode as its underlying value.
    virtual IAsyncOperation<std::uint8_t> toggle_input_mode() = 0;
    virtual IAsyncOperation<std::uint8_t> current_input_mode() = 0;
    // Any thread, used on shutdown: fails outstanding and later I/O.
    virtual void abort() noexcept = 0;
};

}  // namespace tsf
