#include "lora_dataset_builder.hpp"

#include <ime-core/encoding_tables.hpp>
#include <rfl/json.hpp>
#include <utf8/cpp20.h>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <unordered_set>

namespace llavon::service {
namespace {

struct ModelConfig {
    std::int64_t vocab_size;
    std::int64_t max_position_embeddings;
};

struct PaddingEntry {
    std::optional<std::string> syllable;
    std::optional<int> tone;
    std::optional<std::string> literal;
};

std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("unable to open JSON file");
    return std::string(std::istreambuf_iterator<char>(input),
                       std::istreambuf_iterator<char>());
}

void append_integer_array(std::string& output,
                          const std::vector<std::int64_t>& values) {
    output.push_back('[');
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index != 0) output.push_back(',');
        output += std::to_string(values[index]);
    }
    output.push_back(']');
}

void append_repeated_array(std::string& output, std::size_t count,
                           std::int64_t value) {
    output.push_back('[');
    for (std::size_t index = 0; index < count; ++index) {
        if (index != 0) output.push_back(',');
        output += std::to_string(value);
    }
    output.push_back(']');
}

std::string reading_with_tone(const PaddingEntry& entry) {
    if (!entry.syllable || !entry.tone) return {};
    std::string result = *entry.syllable;
    switch (*entry.tone) {
        case 1: result += " "; break;
        case 2: result += "ˊ"; break;
        case 3: result += "ˇ"; break;
        case 4: result += "ˋ"; break;
        case 5: result += "˙"; break;
        default: return {};
    }
    return result;
}

bool build_row(const TrainingDataRecord& record, const ime::core::EncodingTables& tables,
               std::int32_t maximum_sequence_length, std::string& output) {
    const auto padding = rfl::json::read<std::vector<PaddingEntry>>(record.padding_json);
    if (!padding) return false;
    const std::u32string answer = utf8::utf8to32(utf8::utf16to8(record.answer));
    if (answer.empty() || answer.size() != padding->size()) return false;

    std::vector<ime::core::PaddingEntry> core_padding;
    core_padding.reserve(padding->size());
    std::vector<std::optional<std::u16string>> readings;
    readings.reserve(padding->size());
    for (const auto& [entry, character] : std::views::zip(*padding, answer)) {
        if (entry.literal && !entry.syllable && !entry.tone) {
            const auto characters = utf8::utf8to32(*entry.literal);
            if (characters.size() != 1 || characters.front() != character) return false;
            core_padding.push_back({.chosen = true, .chosen_char = characters.front()});
            readings.push_back(std::nullopt);
            continue;
        }
        if (entry.literal) return false;
        const std::string reading = reading_with_tone(entry);
        if (reading.empty()) return false;
        const auto reading16 = utf8::utf8to16(reading);
        core_padding.push_back({.bopomofo = reading16});
        readings.push_back(reading16);
    }
    std::vector<std::int64_t> tokens;
    try {
        tokens = tables.tokenize(record.context, core_padding);
    } catch (const std::logic_error&) {
        return false;
    }
    const std::size_t prompt_length = tokens.size();

    std::vector<std::optional<std::vector<std::int64_t>>> candidate_masks;
    candidate_masks.reserve(answer.size());
    std::vector<std::int64_t> loss_weights;
    loss_weights.reserve(answer.size());
    for (const auto& [reading, character] : std::views::zip(readings, answer)) {
        if (!reading) {
            tokens.push_back(tables.token_for_character(character));
            candidate_masks.push_back(std::nullopt);
            loss_weights.push_back(0);
            continue;
        }
        const auto candidates = tables.candidates_for_reading(*reading);
        if (candidates.empty()) return false;
        std::vector<std::int64_t> mask;
        std::unordered_set<std::int64_t> seen;
        std::int64_t answer_token = -1;
        for (const auto& [candidate, token] : candidates) {
            if (!seen.insert(token).second) continue;
            mask.push_back(token);
            if (candidate == character) answer_token = token;
        }
        if (answer_token < 0 || mask.empty()) return false;
        tokens.push_back(answer_token);
        candidate_masks.push_back(std::move(mask));
        loss_weights.push_back(1);
    }
    if (std::find(loss_weights.begin(), loss_weights.end(), 1) == loss_weights.end()) return false;
    if (tokens.size() > static_cast<std::size_t>(maximum_sequence_length)) return false;

    output += R"({"tokens":)";
    append_integer_array(output, tokens);
    output += R"(,"labels":)";
    append_integer_array(output, tokens);
    output += R"(,"loss_weights":)";
    output.push_back('[');
    for (std::size_t index = 0; index < prompt_length; ++index) {
        if (index != 0) output.push_back(',');
        output.push_back('0');
    }
    for (const auto weight : loss_weights) {
        output.push_back(',');
        output += std::to_string(weight);
    }
    output += R"(],"attention_mask":)";
    append_repeated_array(output, tokens.size(), 1);
    output += R"(,"candidate_masks":[)";
    for (std::size_t index = 0; index < prompt_length; ++index) {
        if (index != 0) output.push_back(',');
        output += "null";
    }
    for (const auto& mask : candidate_masks) {
        output.push_back(',');
        if (mask) {
            append_integer_array(output, *mask);
        } else {
            output += "null";
        }
    }
    output += "]}\n";
    return true;
}

}  // namespace

LoraDatasetBuildResult write_lora_numeric_dataset(
    const std::vector<TrainingDataRecord>& records,
    const std::filesystem::path& tables_directory,
    const std::filesystem::path& model_config_path,
    const std::filesystem::path& destination,
    std::int32_t maximum_sequence_length) {
    if (records.empty()) throw std::invalid_argument("no pending training records were selected");
    if (maximum_sequence_length <= 1) {
        throw std::invalid_argument("maximum sequence length must be greater than one");
    }

    const auto model_config = rfl::json::read<ModelConfig>(read_file(model_config_path)).value();
    const std::int64_t vocabulary_size = model_config.vocab_size;
    const std::int64_t model_maximum = model_config.max_position_embeddings;
    if (vocabulary_size <= 0 || model_maximum <= 1 ||
        maximum_sequence_length > model_maximum) {
        throw std::invalid_argument("training sequence length exceeds the model configuration");
    }
    const ime::core::EncodingTables tables(tables_directory);
    if (!tables.tokens_fit_vocabulary(vocabulary_size)) {
        throw std::runtime_error("IME token ID is outside the model vocabulary");
    }

    if (!destination.parent_path().empty()) {
        std::filesystem::create_directories(destination.parent_path());
    }
    auto partial = destination;
    partial += L".partial";
    std::error_code error;
    std::filesystem::remove(partial, error);

    LoraDatasetBuildResult result{
        .pad_token_id = tables.pad_token_id(),
        .vocabulary_size = vocabulary_size,
        .model_max_sequence_length = model_maximum,
    };
    try {
        std::ofstream output(partial, std::ios::binary | std::ios::trunc);
        if (!output) throw std::runtime_error("unable to create numeric training dataset");
        for (const auto& record : records) {
            std::string row;
            if (build_row(record, tables, maximum_sequence_length, row)) {
                // Give explicit candidate choices three samples per epoch.
                for (int copy = 0; copy < (record.revice ? 3 : 1); ++copy) {
                    output << row;
                }
                ++result.written;
                result.included_event_ids.push_back(record.event_id);
            } else {
                ++result.skipped;
            }
        }
        output.flush();
        if (!output) throw std::runtime_error("unable to write numeric training dataset");
        output.close();
        if (result.written == 0) {
            throw std::runtime_error("selected commits contain no trainable Bopomofo rows");
        }
        std::filesystem::remove(destination, error);
        error.clear();
        std::filesystem::rename(partial, destination, error);
        if (error) throw std::filesystem::filesystem_error(
            "unable to publish numeric training dataset", partial, destination, error);
        return result;
    } catch (...) {
        std::filesystem::remove(partial, error);
        throw;
    }
}

}  // namespace llavon::service
