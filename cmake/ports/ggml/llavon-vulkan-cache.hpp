#pragma once

// Application-owned extension to the pinned ggml Vulkan backend. Vulkan-Hpp
// and its dispatcher have already been declared by ggml-vulkan.cpp.
#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace llavon {

class VulkanPipelineCache final {
    using Digest = std::array<std::uint8_t, 32>;
    static constexpr std::uint32_t maximum_bytes = 128u * 1024u * 1024u;
    struct Header {
        std::array<char, 8> magic{'L','L','A','V','V','K','C','1'};
        std::uint32_t version = 1;
        std::uint32_t vendor = 0;
        std::uint32_t device = 0;
        std::uint32_t driver = 0;
        std::uint32_t pointer_bytes = sizeof(void*);
        std::uint32_t size = 0;
        std::array<std::uint8_t, VK_UUID_SIZE> uuid{};
        Digest digest{};
    };
    static_assert(sizeof(Header) == 80);
    static_assert(std::endian::native == std::endian::little);

    vk::Device device_;
    vk::PipelineCache cache_;
    Header identity_;
    std::filesystem::path path_;
    std::mutex mutex_;
    std::optional<Digest> saved_digest_;
    bool dirty_ = false;

    static std::wstring environment(const wchar_t* name) {
        const DWORD capacity = GetEnvironmentVariableW(name, nullptr, 0);
        if (capacity == 0) return {};
        std::wstring value(capacity, L'\0');
        const DWORD size = GetEnvironmentVariableW(name, value.data(), capacity);
        if (size == 0 || size >= capacity) return {};
        value.resize(size);
        return value;
    }

    static std::optional<Digest> digest(std::span<const std::uint8_t> data) noexcept {
        BCRYPT_ALG_HANDLE algorithm = nullptr;
        if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return {};
        Digest result{};
        const auto status = BCryptHash(algorithm, nullptr, 0,
            const_cast<PUCHAR>(data.data()), static_cast<ULONG>(data.size()),
            result.data(), static_cast<ULONG>(result.size()));
        BCryptCloseAlgorithmProvider(algorithm, 0);
        if (status < 0) return {};
        return result;
    }

    bool valid_vulkan_header(std::span<const std::uint8_t> data) const noexcept {
        if (data.size() < 32) return false;
        std::array<std::uint32_t, 4> fields{};
        std::memcpy(fields.data(), data.data(), sizeof(fields));
        return fields[0] == 32 && fields[1] == VK_PIPELINE_CACHE_HEADER_VERSION_ONE &&
               fields[2] == identity_.vendor && fields[3] == identity_.device &&
               std::equal(identity_.uuid.begin(), identity_.uuid.end(), data.begin() + 16);
    }

    std::vector<std::uint8_t> load() {
        std::ifstream file(path_, std::ios::binary | std::ios::ate);
        if (!file) return {};
        const auto file_size = file.tellg();
        if (file_size < static_cast<std::streamoff>(sizeof(Header)) ||
            file_size > static_cast<std::streamoff>(maximum_bytes + sizeof(Header))) return {};
        file.seekg(0);
        Header header{};
        file.read(reinterpret_cast<char*>(&header), sizeof(header));
        if (!file || header.magic != identity_.magic || header.version != identity_.version ||
            header.vendor != identity_.vendor || header.device != identity_.device ||
            header.driver != identity_.driver || header.pointer_bytes != identity_.pointer_bytes ||
            header.uuid != identity_.uuid || header.size < 32 || header.size > maximum_bytes ||
            file_size != static_cast<std::streamoff>(sizeof(Header) + header.size)) return {};
        std::vector<std::uint8_t> data(header.size);
        file.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()));
        const auto hash = digest(data);
        if (!file || !valid_vulkan_header(data) || !hash || *hash != header.digest) return {};
        saved_digest_ = hash;
        return data;
    }

public:
    VulkanPipelineCache() = default;
    VulkanPipelineCache(const VulkanPipelineCache&) = delete;
    VulkanPipelineCache& operator=(const VulkanPipelineCache&) = delete;

    void initialize(vk::Device device, const vk::PhysicalDeviceProperties& properties) noexcept {
        device_ = device;
        try {
            if (!environment(L"GGML_VK_PIPELINE_CACHE_DISABLE").empty()) return;
            auto directory = environment(L"GGML_VK_PIPELINE_CACHE_DIR");
            if (directory.empty()) {
                const auto local = environment(L"LOCALAPPDATA");
                if (local.empty()) return;
                directory = (std::filesystem::path(local) / L"Llavon IME" / L"vulkan-cache").wstring();
            }
            identity_.vendor = properties.vendorID;
            identity_.device = properties.deviceID;
            identity_.driver = properties.driverVersion;
            std::copy(properties.pipelineCacheUUID.begin(), properties.pipelineCacheUUID.end(), identity_.uuid.begin());
            std::string uuid;
            for (const auto byte : identity_.uuid) uuid += std::format("{:02x}", byte);
            path_ = std::filesystem::path(directory) / std::format(
                "ggml-0.11.1-v1-{:x}-{:x}-{:x}-{}-{}.bin",
                identity_.vendor, identity_.device, identity_.driver, sizeof(void*), uuid);
            auto data = load();
            try {
                cache_ = device_.createPipelineCache(vk::PipelineCacheCreateInfo({}, data.size(), data.data()));
            } catch (const vk::SystemError&) {
                // A driver can reject otherwise well-formed, obsolete data.
                data.clear();
                saved_digest_.reset();
                cache_ = device_.createPipelineCache({});
            }
            std::fprintf(stderr, "[VK_CACHE] %s bytes=%zu\n", data.empty() ? "miss" : "loaded", data.size());
        } catch (...) {
            // Filesystem/cache failures must not prevent inference.
            std::fprintf(stderr, "[VK_CACHE] unavailable; continuing without disk reuse\n");
        }
    }

    vk::Pipeline create(const vk::ComputePipelineCreateInfo& info) {
        const std::lock_guard lock(mutex_);
        auto pipeline = device_.createComputePipeline(cache_, info).value;
        dirty_ = true;
        return pipeline;
    }

    // Explicitly called after model preparation, so a crash later in the
    // session does not discard the cache. Never called on the prediction path.
    void save() noexcept {
        try {
            const std::lock_guard lock(mutex_);
            if (!cache_ || path_.empty() || !dirty_) return;
            std::size_t size = 0;
            if (device_.getPipelineCacheData(cache_, &size, nullptr) != vk::Result::eSuccess ||
                size < 32 || size > maximum_bytes) return;
            std::vector<std::uint8_t> data(size);
            if (device_.getPipelineCacheData(cache_, &size, data.data()) != vk::Result::eSuccess ||
                size > data.size()) return;
            data.resize(size);
            if (!valid_vulkan_header(data)) return;
            const auto hash = digest(data);
            if (!hash) return;
            if (saved_digest_ == hash) { dirty_ = false; return; }
            Header header = identity_;
            header.size = static_cast<std::uint32_t>(size);
            header.digest = *hash;
            std::filesystem::create_directories(path_.parent_path());
            static std::atomic_uint64_t sequence{};
            auto temporary = path_;
            temporary += std::format(L".{}.{}.{}.tmp", GetCurrentProcessId(), GetTickCount64(), sequence.fetch_add(1));
            try {
                {
                    std::ofstream file(temporary, std::ios::binary | std::ios::out | std::ios::noreplace);
                    file.write(reinterpret_cast<const char*>(&header), sizeof(header));
                    file.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
                    file.close();
                    if (!file) throw std::runtime_error("pipeline cache write failed");
                }
                if (!MoveFileExW(temporary.c_str(), path_.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
                    throw std::runtime_error("pipeline cache replacement failed");
                }
            } catch (...) {
                std::error_code ignored;
                std::filesystem::remove(temporary, ignored);
                throw;
            }
            saved_digest_ = hash;
            dirty_ = false;
            std::fprintf(stderr, "[VK_CACHE] saved bytes=%zu\n", size);
        } catch (...) {
            std::fprintf(stderr, "[VK_CACHE] save failed; inference remains available\n");
        }
    }

    void close() noexcept {
        save();
        if (cache_) device_.destroyPipelineCache(cache_);
        cache_ = nullptr;
    }
};

} // namespace llavon
