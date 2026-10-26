#include "winrt_http.hpp"

#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Web.Http.Headers.h>
#include <winrt/Windows.Web.Http.h>

#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace llavon::service {
namespace {

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

std::runtime_error winrt_http_error(const winrt::hresult_error& error) {
    return std::runtime_error("WinRT HTTP: " + winrt::to_string(error.message()));
}

winrt::Windows::Foundation::IAsyncAction get_texts_async(
    std::vector<std::wstring> urls, std::size_t max_size,
    std::shared_ptr<std::vector<HttpTextResult>> results) {
    auto cancellation = co_await winrt::get_cancellation_token();
    std::vector<winrt::Windows::Foundation::IAsyncAction> requests;
    requests.reserve(urls.size());
    for (std::size_t index = 0; index < urls.size(); ++index) {
        requests.push_back(get_stream_async(std::move(urls[index]),
            [results, index, max_size](const std::uint8_t* bytes, std::uint32_t count,
                                       std::uint64_t, std::uint64_t) {
                auto& body = *(*results)[index];
                if (body.size() + count > max_size)
                    throw std::runtime_error("HTTP response is too large");
                body.append(reinterpret_cast<const char*>(bytes), count);
            }));
    }
    cancellation.callback([requests] {
        for (const auto& request : requests) request.Cancel();
    });
    for (std::size_t index = 0; index < requests.size(); ++index) {
        try {
            co_await requests[index];
        } catch (const winrt::hresult_error& error) {
            (*results)[index] = std::unexpected(
                std::make_exception_ptr(winrt_http_error(error)));
        } catch (...) {
            (*results)[index] = std::unexpected(std::current_exception());
        }
    }
}

}  // namespace

void WinrtHttpTransfer::get_stream(std::wstring url, HttpChunkSink consume) {
    run([&] { return get_stream_async(std::move(url), std::move(consume)); });
}

std::vector<HttpTextResult> WinrtHttpTransfer::get_texts(
    std::vector<std::wstring> urls, std::size_t max_size) {
    auto results = std::make_shared<std::vector<HttpTextResult>>(urls.size());
    run([&] { return get_texts_async(std::move(urls), max_size, results); });
    return std::move(*results);
}

void WinrtHttpTransfer::run(
    const std::function<winrt::Windows::Foundation::IAsyncAction()>& start) {
    try {
        auto action = start();
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
        throw winrt_http_error(error);
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
