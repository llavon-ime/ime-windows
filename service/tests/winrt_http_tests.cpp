#include "service/winrt_http.hpp"

#include <asio.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

int main() {
    using asio::ip::tcp;
    asio::io_context io;
    tcp::acceptor listener(io, tcp::endpoint(asio::ip::address_v4::loopback(), 0));
    const auto port = listener.local_endpoint().port();
    const auto body = std::make_shared<std::string>(2 * 1024 * 1024 + 17, 'x');
    std::atomic_bool first_chunk_seen{false};
    std::atomic_bool sent_remainder{false};
    bool served = false;
    listener.async_accept([&](const asio::error_code& error, tcp::socket peer) {
        if (error) return;
        auto socket = std::make_shared<tcp::socket>(std::move(peer));
        auto request = std::make_shared<asio::streambuf>();
        asio::async_read_until(*socket, *request, "\r\n\r\n",
            [&, socket, request](const asio::error_code& read_error, std::size_t) {
                if (read_error) return;
                const auto first_half = body->size() / 2;
                auto response = std::make_shared<std::string>(
                    "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\n"
                    "Content-Length: " + std::to_string(body->size()) +
                    "\r\nConnection: close\r\n\r\n" + body->substr(0, first_half));
                asio::async_write(*socket, asio::buffer(*response),
                    [&, socket, response, first_half](
                        const asio::error_code& write_error, std::size_t) {
                        if (write_error) return;
                        const auto deadline = std::chrono::steady_clock::now() +
                                              std::chrono::seconds(3);
                        while (!first_chunk_seen.load(std::memory_order_acquire) &&
                               std::chrono::steady_clock::now() < deadline) {
                            std::this_thread::sleep_for(std::chrono::milliseconds(10));
                        }
                        sent_remainder.store(true, std::memory_order_release);
                        auto remainder = std::make_shared<std::string>(
                            body->substr(first_half));
                        asio::async_write(*socket, asio::buffer(*remainder),
                            [&, socket, remainder](const asio::error_code& final_error,
                                                   std::size_t) {
                                served = !final_error;
                            });
                    });
            });
    });
    std::thread server([&] { io.run_for(std::chrono::seconds(15)); });

    llavon::service::WinrtHttpTransfer transfer;
    std::uint64_t last_received = 0;
    std::size_t chunks = 0;
    bool valid = true;
    bool streamed_before_complete = false;
    try {
        transfer.get_stream(
            L"http://127.0.0.1:" + std::to_wstring(port) + L"/model",
            [&](const std::uint8_t* bytes, std::uint32_t count,
                std::uint64_t received, std::uint64_t total) {
                valid = valid && count > 0 && count <= 256 * 1024 &&
                        received == last_received + count && total == body->size();
                for (std::uint32_t index = 0; index < count; ++index) {
                    if (bytes[index] != static_cast<std::uint8_t>('x')) valid = false;
                }
                if (chunks == 0) {
                    streamed_before_complete =
                        !sent_remainder.load(std::memory_order_acquire);
                    first_chunk_seen.store(true, std::memory_order_release);
                }
                last_received = received;
                ++chunks;
            });
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        valid = false;
    }
    io.stop();
    server.join();
    if (!valid || !served || !streamed_before_complete ||
        chunks < 2 || last_received != body->size()) {
        std::cerr << "WinRT HTTP did not stream the complete response\n";
        return 1;
    }

    asio::io_context cancel_io;
    tcp::acceptor stalled_listener(
        cancel_io, tcp::endpoint(asio::ip::address_v4::loopback(), 0));
    const auto stalled_port = stalled_listener.local_endpoint().port();
    std::atomic_bool accepted{false};
    std::shared_ptr<tcp::socket> stalled_socket;
    stalled_listener.async_accept(
        [&](const asio::error_code& error, tcp::socket peer) {
            if (error) return;
            stalled_socket = std::make_shared<tcp::socket>(std::move(peer));
            accepted.store(true, std::memory_order_release);
        });
    std::thread stalled_server([&] { cancel_io.run_for(std::chrono::seconds(5)); });
    llavon::service::WinrtHttpTransfer stalled_transfer;
    bool cancelled = false;
    std::thread client([&] {
        try {
            stalled_transfer.get_stream(
                L"http://127.0.0.1:" + std::to_wstring(stalled_port) + L"/stall",
                [](const std::uint8_t*, std::uint32_t,
                   std::uint64_t, std::uint64_t) {});
        } catch (const std::exception& error) {
            cancelled = std::string(error.what()) == "operation cancelled";
        }
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!accepted.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    stalled_transfer.cancel();
    client.join();
    cancel_io.stop();
    stalled_server.join();
    if (!accepted.load(std::memory_order_acquire) || !cancelled) {
        std::cerr << "WinRT HTTP request did not cancel while waiting for headers\n";
        return 2;
    }

    bool reported_winrt_error = false;
    try {
        transfer.get_stream(
            L"not-a-valid-http-url",
            [](const std::uint8_t*, std::uint32_t,
               std::uint64_t, std::uint64_t) {});
    } catch (const std::runtime_error& error) {
        reported_winrt_error = std::string(error.what()).starts_with("WinRT HTTP:");
    }
    if (!reported_winrt_error) {
        std::cerr << "WinRT HTTP exception escaped the worker error boundary\n";
        return 3;
    }
    return 0;
}
