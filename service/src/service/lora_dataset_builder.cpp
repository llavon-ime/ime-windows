#include "lora_dataset_builder.hpp"

#include <rfl/json.hpp>
#include <utf8/cpp20.h>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace llavon::service {
namespace {

using TokenMap = std::unordered_map<std::string, std::int64_t>;
using CandidateMap = std::unordered_map<std::string, std::vector<std::string>>;

struct ModelConfig {
    std::int64_t vocab_size;
    std::int64_t max_position_embeddings;
};

struct PaddingEntry {
    std::optional<std::string> syllable;
    std::optional<int> tone;
};

std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("unable to open JSON file");
    return std::string(std::istreambuf_iterator<char>(input),
                       std::istreambuf_iterator<char>());
}

std::int64_t required_token(const TokenMap& map, std::string_view name) {
    const auto found = map.find(std::string(name));
    if (found == map.end()) {
        throw std::runtime_error("IME token table is missing a required token");
    }
    return found->second;
}

bool is_latin(char32_t value) {
    return value >= U'a' && value <= U'z' || value >= U'A' && value <= U'Z' ||
           value >= U'0' && value <= U'9' || value == U'-' || value == U'_' ||
           value == U'+';
}

char lower_ascii(char32_t value) {
    return static_cast<char>(value >= U'A' && value <= U'Z' ? value ^ 0x20 : value);
}

std::string code_point_utf8(char32_t value) {
    std::string result;
    utf8::append(value, result);
    return result;
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

struct RuntimeTables {
    TokenMap characters;
    TokenMap latin;
    TokenMap special;
    TokenMap bopomofo;
    CandidateMap candidates;
    std::int64_t pad = 0;
    std::int64_t bos = 0;
    std::int64_t sep = 0;
    std::int64_t unknown = 0;
    std::int64_t space = 0;
    std::int64_t latin_unknown = 0;
};

RuntimeTables load_tables(const std::filesystem::path& directory,
                          std::int64_t vocabulary_size) {
    const auto tokens = directory / L"tokens";
    RuntimeTables tables{
        .characters = rfl::json::load<TokenMap>((tokens / L"chars.json").string()).value(),
        .latin = rfl::json::load<TokenMap>((tokens / L"latin.json").string()).value(),
        .special = rfl::json::load<TokenMap>(
            (tokens / L"special_tokens.json").string()).value(),
        .bopomofo = rfl::json::load<TokenMap>((tokens / L"bpmf.json").string()).value(),
        .candidates = rfl::json::load<CandidateMap>(
            (directory / L"bopomofo_char.json").string()).value(),
    };
    tables.pad = required_token(tables.special, "<PAD>");
    tables.bos = required_token(tables.special, "<BOS>");
    tables.sep = required_token(tables.special, "<SEP>");
    tables.unknown = required_token(tables.special, "<UNK>");
    tables.space = required_token(tables.special, "<SP>");
    tables.latin_unknown = required_token(tables.special, "<LATIN>");

    for (const auto* map : {&tables.characters, &tables.latin,
                            &tables.special, &tables.bopomofo}) {
        for (const auto& [name, id] : *map) {
            static_cast<void>(name);
            if (id < 0 || id >= vocabulary_size) {
                throw std::runtime_error("IME token ID is outside the model vocabulary");
            }
        }
    }
    return tables;
}

std::vector<std::int64_t> tokenize_context(const std::u16string& context,
                                           const RuntimeTables& tables) {
    const std::u32string characters = utf8::utf8to32(utf8::utf16to8(context));
    std::vector<std::int64_t> result;
    result.reserve(characters.size());
    for (std::size_t index = 0; index < characters.size(); ++index) {
        const char32_t value = characters[index];
        if (value == U' ') {
            result.push_back(tables.space);
            continue;
        }
        const auto character = tables.characters.find(code_point_utf8(value));
        if (character != tables.characters.end()) {
            result.push_back(character->second);
            continue;
        }
        if (is_latin(value)) {
            std::string word;
            while (index < characters.size() && is_latin(characters[index])) {
                word.push_back(lower_ascii(characters[index]));
                ++index;
            }
            --index;
            const auto latin = tables.latin.find(word);
            result.push_back(latin == tables.latin.end()
                                 ? tables.latin_unknown
                                 : latin->second);
            continue;
        }
        result.push_back(tables.unknown);
    }
    const auto first_known = std::find_if(result.begin(), result.end(),
        [&](std::int64_t token) { return token != tables.unknown; });
    result.erase(result.begin(), first_known);
    return result;
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

bool build_row(const TrainingDataRecord& record, const RuntimeTables& tables,
               std::int32_t maximum_sequence_length, std::string& output) {
    const auto padding = rfl::json::read<std::vector<PaddingEntry>>(record.padding_json);
    if (!padding) return false;
    const std::u32string answer = utf8::utf8to32(utf8::utf16to8(record.answer));
    if (answer.empty() || answer.size() != padding->size()) return false;

    std::vector<std::string> readings;
    readings.reserve(padding->size());
    for (const auto& entry : *padding) {
        const std::string reading = reading_with_tone(entry);
        if (reading.empty()) return false;
        readings.push_back(reading);
    }

    std::vector<std::int64_t> tokens{tables.bos};
    auto context_tokens = tokenize_context(record.context, tables);
    tokens.insert(tokens.end(), context_tokens.begin(), context_tokens.end());
    for (const auto& reading : readings) {
        const auto token = tables.bopomofo.find("<" + reading + ">");
        if (token == tables.bopomofo.end()) return false;
        tokens.push_back(token->second);
    }
    tokens.push_back(tables.sep);
    const std::size_t prompt_length = tokens.size();

    std::vector<std::vector<std::int64_t>> candidate_masks;
    candidate_masks.reserve(answer.size());
    for (std::size_t position = 0; position < answer.size(); ++position) {
        const auto candidates = tables.candidates.find(readings[position]);
        if (candidates == tables.candidates.end()) return false;
        std::vector<std::int64_t> mask;
        std::unordered_set<std::int64_t> seen;
        std::int64_t answer_token = -1;
        for (const auto& candidate_text : candidates->second) {
            const std::u32string candidate = utf8::utf8to32(candidate_text);
            if (candidate.empty()) continue;
            const auto token = tables.characters.find(code_point_utf8(candidate.front()));
            if (token == tables.characters.end() || !seen.insert(token->second).second) {
                continue;
            }
            mask.push_back(token->second);
            if (candidate.front() == answer[position]) answer_token = token->second;
        }
        if (answer_token < 0 || mask.empty()) return false;
        tokens.push_back(answer_token);
        candidate_masks.push_back(std::move(mask));
    }
    if (tokens.size() > static_cast<std::size_t>(maximum_sequence_length)) return false;

    output += R"({"tokens":)";
    append_integer_array(output, tokens);
    output += R"(,"labels":)";
    append_integer_array(output, tokens);
    output += R"(,"loss_weights":)";
    output.push_back('[');
    for (std::size_t index = 0; index < tokens.size(); ++index) {
        if (index != 0) output.push_back(',');
        output.push_back(index < prompt_length ? '0' : '1');
    }
    output += R"(],"attention_mask":)";
    append_repeated_array(output, tokens.size(), 1);
    output += R"(,"candidate_masks":[)";
    for (std::size_t index = 0; index < prompt_length; ++index) {
        if (index != 0) output.push_back(',');
        output += "null";
    }
    for (const auto& mask : candidate_masks) {
        if (prompt_length != 0 || &mask != &candidate_masks.front()) output.push_back(',');
        append_integer_array(output, mask);
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
    const RuntimeTables tables = load_tables(tables_directory, vocabulary_size);

    if (!destination.parent_path().empty()) {
        std::filesystem::create_directories(destination.parent_path());
    }
    auto partial = destination;
    partial += L".partial";
    std::error_code error;
    std::filesystem::remove(partial, error);

    LoraDatasetBuildResult result{
        .pad_token_id = tables.pad,
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
