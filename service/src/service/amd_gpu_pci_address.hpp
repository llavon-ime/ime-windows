#pragma once

#include <array>
#include <charconv>
#include <optional>
#include <string_view>

namespace llavon::service {

struct AmdPciAddress {
    unsigned bus;
    unsigned device;
    unsigned function;
    bool operator==(const AmdPciAddress&) const = default;
};

constexpr std::optional<AmdPciAddress> amd_pci_address(std::string_view id) noexcept {
    std::array<unsigned, 4> address{};
    constexpr std::array separators{':', ':', '.'};
    for (std::size_t i = 0; i < address.size(); ++i) {
        const auto length = i < separators.size() ? id.find(separators[i]) : id.size();
        if (length == std::string_view::npos || length == 0) return std::nullopt;
        const auto [end, error] = std::from_chars(id.data(), id.data() + length, address[i], 16);
        if (error != std::errc{} || end != id.data() + length) return std::nullopt;
        id.remove_prefix(length + (i < separators.size() ? 1 : 0));
    }
    // AMD's topology extension has no PCI domain. Never guess on another domain.
    if (address[0] != 0 || address[1] > 255 || address[2] > 31 || address[3] > 7) {
        return std::nullopt;
    }
    return AmdPciAddress{address[1], address[2], address[3]};
}

} // namespace llavon::service
