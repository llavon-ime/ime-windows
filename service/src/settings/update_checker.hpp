#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <thread>

namespace llavon::settings {

enum class ReleaseChannel {
    stable,
    rolling,
};

enum class UpdateCheckStatus {
    update_available,
    up_to_date,
    local_newer,
    development_build,
    not_published,
    failed,
};

struct ChannelCheckResult {
    ReleaseChannel channel = ReleaseChannel::rolling;
    UpdateCheckStatus status = UpdateCheckStatus::failed;
    std::uint64_t latest_build = 0;
    std::uint64_t major_update_build = 0;
    std::wstring major_update_message;
    std::wstring latest_commit;
    std::wstring latest_version;
    std::wstring release_url;
    std::wstring setup_url;
    std::wstring setup_sha256;
    std::uint64_t setup_size = 0;
    std::wstring error_message;
};

struct UpdateCheckResult {
    std::uint64_t current_build = 0;
    std::wstring current_commit;
    std::wstring current_version;
    ChannelCheckResult stable{.channel = ReleaseChannel::stable};
    ChannelCheckResult rolling{.channel = ReleaseChannel::rolling};

    const ChannelCheckResult& channel(ReleaseChannel which) const noexcept {
        return which == ReleaseChannel::stable ? stable : rolling;
    }
    ChannelCheckResult& channel(ReleaseChannel which) noexcept {
        return which == ReleaseChannel::stable ? stable : rolling;
    }
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
    static std::wstring_view installed_version() noexcept;
    static UpdateCheckResult check_now() noexcept;
    bool check_async(Completion completion);

private:
    std::atomic_bool checking_{false};
    std::thread worker_;
};

}  // namespace llavon::settings
