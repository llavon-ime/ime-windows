#include "winrt_http.hpp"

#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Web.Http.Headers.h>
#include <winrt/Windows.Web.Http.h>

#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace llavon::service {
namespace {

class WinrtApartment final {
public:
    WinrtApartment() { winrt::init_apartment(winrt::apartment_type::multi_threaded); }
    ~WinrtApartment() { winrt::uninit_apartment(); }
    WinrtApartment(const WinrtApartment&) = delete;
    WinrtApartment& operator=(const WinrtApartment&) = delete;
};

winrt::Windows::Foundation::IAsyncAction get_stream_async(
    std::wstring url, HttpChunkSink consume) {
    using namespace winrt::Windows::Storage::Streams;
    using namespace winrt::Windows::Web::Http;

    auto cancellation = co_await winrt::get_cancellation_token();
    HttpClient client;
    client.DefaultRequestHeaders().UserAgent().ParseAdd(L"llavon-ime/1.0");
    auto request = client.GetAsync(winrt::Windows::Foundation::Uri(winrt::hstring(url)),
                                   HttpCompletionOption::ResponseHeadersRead);
    cancellation.callback([request] { request.Cancel(); });
    const auto response = co_await request;
    if (!response.IsSuccessStatusCode()) {
        throw std::runtime_error("HTTP " +
                                 std::to_string(static_cast<int>(response.StatusCode())));
    }
    const auto length = response.Content().Headers().ContentLength();
    const std::uint64_t total = length ? length.Value() : 0;

    auto open_stream = response.Content().ReadAsInputStreamAsync();
    cancellation.callback([open_stream] { open_stream.Cancel(); });
    const auto stream = co_await open_stream;
    DataReader reader(stream);
    reader.InputStreamOptions(InputStreamOptions::Partial);
    std::vector<std::uint8_t> buffer(256 * 1024);
    std::uint64_t received = 0;
    for (;;) {
        auto load = reader.LoadAsync(static_cast<std::uint32_t>(buffer.size()));
        cancellation.callback([load] { load.Cancel(); });
        const auto count = co_await load;
        if (count == 0) break;
        winrt::array_view<std::uint8_t> bytes(buffer.data(), buffer.data() + count);
        reader.ReadBytes(bytes);
        received += count;
        consume(buffer.data(), count, received, total);
    }
    if (total != 0 && received != total) {
        throw std::runtime_error("HTTP response is incomplete");
    }
}

}  // namespace

void WinrtHttpTransfer::get_stream(std::wstring url, HttpChunkSink consume) {
    WinrtApartment apartment;
    try {
        auto action = get_stream_async(std::move(url), std::move(consume));
        {
            std::lock_guard lock(mutex_);
            active_ = action;
            if (cancelled_) active_.Cancel();
        }
        try {
            action.get(); // Only the dedicated service worker blocks here.
        } catch (...) {
            std::lock_guard lock(mutex_);
            active_ = nullptr;
            throw;
        }
        std::lock_guard lock(mutex_);
        active_ = nullptr;
        if (cancelled_) throw std::runtime_error("operation cancelled");
    } catch (const winrt::hresult_error& error) {
        std::lock_guard lock(mutex_);
        active_ = nullptr;
        if (cancelled_) throw std::runtime_error("operation cancelled");
        throw std::runtime_error("WinRT HTTP: " + winrt::to_string(error.message()));
    }
}

void WinrtHttpTransfer::cancel() noexcept {
    std::lock_guard lock(mutex_);
    cancelled_ = true;
    try {
        if (active_) active_.Cancel();
    } catch (...) {
        // Cancellation is best effort; the worker still checks its flag.
    }
}

void WinrtHttpTransfer::reset() noexcept {
    std::lock_guard lock(mutex_);
    cancelled_ = false;
}

}  // namespace llavon::service
