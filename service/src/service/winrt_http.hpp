#pragma once

#include <cstdint>
#include <exception>
#include <expected>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include <winrt/Windows.Foundation.h>

namespace llavon::service {

using HttpChunkSink = std::function<void(
    const std::uint8_t* bytes, std::uint32_t count,
    std::uint64_t received, std::uint64_t total)>;

using HttpTextResult = std::expected<std::string, std::exception_ptr>;

// Only the dedicated service worker waits here; WinRT HTTP operations use
// co_await, and cancel() interrupts the currently awaited operation. The host
// process must keep an MTA alive (see ProcessMta).
class WinrtHttpTransfer final {
public:
    void get_stream(std::wstring url, HttpChunkSink consume);
    // Starts every request before awaiting any of them. A failed request is
    // reported in its own slot; only cancellation fails the whole call.
    std::vector<HttpTextResult> get_texts(std::vector<std::wstring> urls,
                                          std::size_t max_size);
    void cancel() noexcept;
    void reset() noexcept;

private:
    void run(const std::function<winrt::Windows::Foundation::IAsyncAction()>& start);

    std::mutex mutex_;
    winrt::Windows::Foundation::IAsyncAction active_{nullptr};
    bool cancelled_ = false;
};

}  // namespace llavon::service
