#include "custom_name_matcher.hpp"

#include <algorithm>
#include <queue>
#include <utility>

namespace llavon::service {

CustomNameMatcher::CustomNameMatcher(const std::vector<CustomNameSetting>& settings)
    : automaton_(compile(settings)) {}

void CustomNameMatcher::replace(const std::vector<CustomNameSetting>& settings) {
    automaton_.store(compile(settings), std::memory_order_release);
}

std::vector<std::optional<char32_t>> CustomNameMatcher::apply(
    std::vector<llavon::ime::core::PaddingEntry>& padding) const {
    std::vector<std::optional<char32_t>> replacements(padding.size());
    const auto automaton = automaton_.load(std::memory_order_acquire);
    if (padding.empty() || automaton->rules.empty()) return replacements;

    // Keep only the best match for each start position. The final left-to-right
    // pass then gives leftmost-longest, with settings order as the last tie-breaker.
    std::vector<std::optional<std::size_t>> best_matches(padding.size());
    std::size_t state = 0;
    for (std::size_t index = 0; index < padding.size(); ++index) {
        const auto& entry = padding[index];
        if (entry.chosen) {
            state = 0;
            continue;
        }

        const auto token = automaton->tokens.find(entry.bopomofo);
        if (token == automaton->tokens.end()) {
            state = 0;
            continue;
        }

        while (state != 0 && !automaton->nodes[state].next.contains(token->second)) {
            state = automaton->nodes[state].failure;
        }
        if (const auto transition = automaton->nodes[state].next.find(token->second);
            transition != automaton->nodes[state].next.end()) {
            state = transition->second;
        } else {
            state = 0;
        }

        for (const auto rule_index : automaton->nodes[state].outputs) {
            const auto& rule = automaton->rules[rule_index];
            const std::size_t start = index + 1 - rule.characters.size();
            auto& current = best_matches[start];
            if (!current) {
                current = rule_index;
                continue;
            }

            const auto& previous = automaton->rules[*current];
            if (rule.characters.size() > previous.characters.size() ||
                (rule.characters.size() == previous.characters.size() &&
                 rule.order < previous.order)) {
                current = rule_index;
            }
        }
    }

    for (std::size_t start = 0; start < padding.size();) {
        if (!best_matches[start]) {
            ++start;
            continue;
        }

        const auto& matched = automaton->rules[*best_matches[start]];
        for (std::size_t offset = 0; offset < matched.characters.size(); ++offset) {
            auto& entry = padding[start + offset];
            entry.chosen = true;
            entry.chosen_char = matched.characters[offset];
            replacements[start + offset] = matched.characters[offset];
        }
        start += matched.characters.size();
    }
    return replacements;
}

std::shared_ptr<const CustomNameMatcher::Automaton> CustomNameMatcher::compile(
    const std::vector<CustomNameSetting>& settings) {
    auto automaton = std::make_shared<Automaton>();
    automaton->nodes.emplace_back();
    automaton->rules.reserve(settings.size());
    TokenId next_token = 0;

    for (std::size_t order = 0; order < settings.size(); ++order) {
        const auto& setting = settings[order];
        if (setting.name.empty() || setting.name.size() != setting.readings.size()) continue;
        if (std::ranges::any_of(setting.readings, [](const auto& reading) {
                return reading.empty();
            })) {
            continue;
        }

        const std::size_t rule_index = automaton->rules.size();
        automaton->rules.push_back(Rule{
            .characters = setting.name,
            .order = order,
        });

        std::size_t state = 0;
        for (const auto& reading : setting.readings) {
            auto [token, inserted] = automaton->tokens.try_emplace(reading, next_token);
            if (inserted) ++next_token;

            const auto transition = automaton->nodes[state].next.find(token->second);
            if (transition != automaton->nodes[state].next.end()) {
                state = transition->second;
                continue;
            }

            const std::size_t child = automaton->nodes.size();
            automaton->nodes.emplace_back();
            automaton->nodes[state].next.emplace(token->second, child);
            state = child;
        }
        automaton->nodes[state].outputs.push_back(rule_index);
    }

    std::queue<std::size_t> pending;
    for (const auto& [token, child] : automaton->nodes[0].next) {
        (void)token;
        pending.push(child);
    }

    while (!pending.empty()) {
        const std::size_t parent = pending.front();
        pending.pop();

        for (const auto& [token, child] : automaton->nodes[parent].next) {
            std::size_t failure = automaton->nodes[parent].failure;
            while (failure != 0 && !automaton->nodes[failure].next.contains(token)) {
                failure = automaton->nodes[failure].failure;
            }
            if (const auto transition = automaton->nodes[failure].next.find(token);
                transition != automaton->nodes[failure].next.end()) {
                failure = transition->second;
            }

            automaton->nodes[child].failure = failure;
            const auto& inherited = automaton->nodes[failure].outputs;
            automaton->nodes[child].outputs.insert(
                automaton->nodes[child].outputs.end(), inherited.begin(), inherited.end());
            pending.push(child);
        }
    }

    return automaton;
}

}  // namespace llavon::service
