#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>

#include "../service/winrt_http.hpp"

namespace llavon::settings {

struct SetupAsset {
    std::wstring url;
    std::wstring sha256;
    std::uint64_t size = 0;
};

enum class UpdateInstallStage {
    downloading,
    launching,
    launched,
    failed,
};

struct UpdateInstallEvent {
    UpdateInstallStage stage = UpdateInstallStage::failed;
    std::uint64_t received = 0;
    std::uint64_t total = 0;
    std::wstring error;
};

class UpdateInstaller final {
public:
    using Callback = std::function<void(UpdateInstallEvent)>;

    UpdateInstaller() = default;
    UpdateInstaller(const UpdateInstaller&) = delete;
    UpdateInstaller& operator=(const UpdateInstaller&) = delete;
    ~UpdateInstaller();

    bool install_async(SetupAsset asset, Callback callback);
    bool installing() const noexcept {
        return installing_.load(std::memory_order_acquire);
    }
    void cancel() noexcept;

private:
    std::atomic_bool installing_{false};
    llavon::service::WinrtHttpTransfer transfer_;
    std::thread worker_;
};

}  // namespace llavon::settings
