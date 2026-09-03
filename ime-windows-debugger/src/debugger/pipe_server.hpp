#pragma once

#include <windows.h>

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace llavon::debugger {

class PipeServer final {
public:
    using ConnectionCallback = std::function<void(int)>;
    using MessageCallback = std::function<void(std::string)>;

    PipeServer(ConnectionCallback connection_callback, MessageCallback message_callback);
    ~PipeServer();

    PipeServer(const PipeServer&) = delete;
    PipeServer& operator=(const PipeServer&) = delete;

private:
    void accept_clients() noexcept;
    void handle_client(HANDLE pipe) noexcept;
    bool read_exact(HANDLE pipe, HANDLE event, void* buffer, std::size_t size) noexcept;
    HANDLE create_pipe() const noexcept;
    void reap_clients() noexcept;

    struct ClientThread {
        std::thread thread;
        std::shared_ptr<std::atomic<bool>> complete;
    };

    ConnectionCallback connection_callback_;
    MessageCallback message_callback_;
    HANDLE stop_event_ = nullptr;
    std::thread accept_thread_;
    std::vector<ClientThread> client_threads_;
};

}  // namespace llavon::debugger
