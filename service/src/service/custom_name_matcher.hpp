#pragma once

#include "user_settings.hpp"

#include <ime-core/core.hpp>

#include <atomic>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace llavon::service {

class CustomNameMatcher final {
public:
    explicit CustomNameMatcher(const std::vector<CustomNameSetting>& settings);

    void replace(const std::vector<CustomNameSetting>& settings);

    // Marks matched entries as chosen for inference and returns the character
    // that must be sent back to the frontend at every replaced position.
    std::vector<std::optional<char32_t>> apply(
        std::vector<llavon::ime::core::PaddingEntry>& padding) const;

private:
    using TokenId = std::size_t;

    struct Rule {
        std::u32string characters;
        std::size_t order = 0;
    };

    struct Node {
        std::unordered_map<TokenId, std::size_t> next;
        std::size_t failure = 0;
        std::vector<std::size_t> outputs;
    };

    struct Automaton {
        std::unordered_map<std::u16string, TokenId> tokens;
        std::vector<Node> nodes;
        std::vector<Rule> rules;
    };

    static std::shared_ptr<const Automaton> compile(
        const std::vector<CustomNameSetting>& settings);

    std::atomic<std::shared_ptr<const Automaton>> automaton_;
};

}  // namespace llavon::service
