#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <thread>

namespace llavon::settings {

enum class UpdateCheckStatus {
    update_available,
    up_to_date,
    local_newer,
    development_build,
    failed,
};

struct UpdateCheckResult {
    UpdateCheckStatus status = UpdateCheckStatus::failed;
    std::uint64_t current_build = 0;
    std::uint64_t latest_build = 0;
    std::wstring current_commit;
    std::wstring latest_commit;
    std::wstring release_url;
    std::wstring error_message;
};

class UpdateChecker final {
public:
    using Completion = std::function<void(UpdateCheckResult)>;

    UpdateChecker() = default;
    UpdateChecker(const UpdateChecker&) = delete;
    UpdateChecker& operator=(const UpdateChecker&) = delete;
    ~UpdateChecker();

    static std::uint64_t installed_build_number() noexcept;
    static std::wstring_view installed_commit() noexcept;
    bool check_async(Completion completion);

private:
    static UpdateCheckResult check_now() noexcept;

    std::atomic_bool checking_{false};
    std::thread worker_;
};

}  // namespace llavon::settings
