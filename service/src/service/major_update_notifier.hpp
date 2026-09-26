#pragma once

#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <stop_token>
#include <thread>

namespace llavon::service {

class MajorUpdateNotifier final {
public:
    explicit MajorUpdateNotifier(std::function<void()> open_settings);
    MajorUpdateNotifier(const MajorUpdateNotifier&) = delete;
    MajorUpdateNotifier& operator=(const MajorUpdateNotifier&) = delete;
    ~MajorUpdateNotifier();

private:
    struct ActivationState;

    void run(std::stop_token stop) noexcept;

    std::shared_ptr<ActivationState> activation_;
    std::mutex wait_mutex_;
    std::condition_variable_any wake_;
    std::jthread worker_;
};

}  // namespace llavon::service
