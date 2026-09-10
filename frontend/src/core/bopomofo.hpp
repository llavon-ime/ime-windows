#pragma once

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <optional>
#include <source_location>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "jsoncons/json.hpp"
#include "system/globals.h"
#include "utf8cpp/utf8/cpp20.h"

namespace tsf {

class Bopomofo {
private:
    static constexpr int VK_0 = 0x30;
    static constexpr int VK_1 = 0x31;
    static constexpr int VK_2 = 0x32;
    static constexpr int VK_3 = 0x33;
    static constexpr int VK_4 = 0x34;
    static constexpr int VK_5 = 0x35;
    static constexpr int VK_6 = 0x36;
    static constexpr int VK_7 = 0x37;
    static constexpr int VK_8 = 0x38;
    static constexpr int VK_9 = 0x39;

    static constexpr int VK_A = 0x41;
    static constexpr int VK_B = 0x42;
    static constexpr int VK_C = 0x43;
    static constexpr int VK_D = 0x44;
    static constexpr int VK_E = 0x45;
    static constexpr int VK_F = 0x46;
    static constexpr int VK_G = 0x47;
    static constexpr int VK_H = 0x48;
    static constexpr int VK_I = 0x49;
    static constexpr int VK_J = 0x4A;
    static constexpr int VK_K = 0x4B;
    static constexpr int VK_L = 0x4C;
    static constexpr int VK_M = 0x4D;
    static constexpr int VK_N = 0x4E;
    static constexpr int VK_O = 0x4F;
    static constexpr int VK_P = 0x50;
    static constexpr int VK_Q = 0x51;
    static constexpr int VK_R = 0x52;
    static constexpr int VK_S = 0x53;
    static constexpr int VK_T = 0x54;
    static constexpr int VK_U = 0x55;
    static constexpr int VK_V = 0x56;
    static constexpr int VK_W = 0x57;
    static constexpr int VK_X = 0x58;
    static constexpr int VK_Y = 0x59;
    static constexpr int VK_Z = 0x5A;

    inline static const std::unordered_map<int, char16_t> vkToBopomofo{{
        // Standard Zhuyin keyboard layout.
        {VK_1, L'ㄅ'},
        {VK_2, L'ㄉ'},
        {VK_5, L'ㄓ'},
        {VK_8, L'ㄚ'},
        {VK_9, L'ㄞ'},
        {VK_0, L'ㄢ'},
        {VK_OEM_MINUS, L'ㄦ'},

        {VK_Q, L'ㄆ'},
        {VK_W, L'ㄊ'},
        {VK_E, L'ㄍ'},
        {VK_R, L'ㄐ'},
        {VK_T, L'ㄔ'},
        {VK_Y, L'ㄗ'},
        {VK_U, L'ㄧ'},
        {VK_I, L'ㄛ'},
        {VK_O, L'ㄟ'},
        {VK_P, L'ㄣ'},

        {VK_A, L'ㄇ'},
        {VK_S, L'ㄋ'},
        {VK_D, L'ㄎ'},
        {VK_F, L'ㄑ'},
        {VK_G, L'ㄕ'},
        {VK_H, L'ㄘ'},
        {VK_J, L'ㄨ'},
        {VK_K, L'ㄜ'},
        {VK_L, L'ㄠ'},
        {VK_OEM_1, L'ㄤ'},

        {VK_Z, L'ㄈ'},
        {VK_X, L'ㄌ'},
        {VK_C, L'ㄏ'},
        {VK_V, L'ㄒ'},
        {VK_B, L'ㄖ'},
        {VK_N, L'ㄙ'},
        {VK_M, L'ㄩ'},
        {VK_OEM_COMMA, L'ㄝ'},
        {VK_OEM_PERIOD, L'ㄡ'},
        {VK_OEM_2, L'ㄥ'},

        // Tone keys.
        {VK_SPACE, L' '},
        {VK_6, L'ˊ'},
        {VK_3, L'ˇ'},
        {VK_4, L'ˋ'},
        {VK_7, L'˙'},
    }};

public:
    inline static const std::unordered_set<char16_t> initial{
        u'ㄅ', u'ㄆ', u'ㄇ', u'ㄈ', u'ㄉ', u'ㄊ', u'ㄋ', u'ㄌ', u'ㄍ', u'ㄎ', u'ㄏ',
        u'ㄐ', u'ㄑ', u'ㄒ', u'ㄓ', u'ㄔ', u'ㄕ', u'ㄖ', u'ㄗ', u'ㄘ', u'ㄙ'};
    inline static const std::unordered_set<char16_t> medial{u'ㄧ', u'ㄨ', u'ㄩ'};
    inline static const std::unordered_set<char16_t> final{
        u'ㄚ', u'ㄛ', u'ㄜ', u'ㄝ', u'ㄞ', u'ㄟ', u'ㄠ', u'ㄡ', u'ㄢ', u'ㄣ', u'ㄤ', u'ㄥ', u'ㄦ'};
    inline static const std::unordered_set<char16_t> tone{u' ', u'ˊ', u'ˇ', u'ˋ', u'˙'};

    static std::optional<char16_t> lookup(int vk) {
        const auto it = vkToBopomofo.find(vk);
        if (it == vkToBopomofo.end()) return std::nullopt;
        return it->second;
    }
};

class HanziMapEngine {
    std::unordered_map<std::u16string, std::vector<char32_t>> mapping;
    std::unordered_set<char32_t> hanzi_set;

public:
    static HanziMapEngine& instance() {
        static HanziMapEngine engine;
        return engine;
    }
    char32_t lookup_first(const std::u16string& bopomofo) {
        if (mapping.contains(bopomofo)) {
            if (mapping[bopomofo].size() == 0) {
                throw std::logic_error("table error");
            }
            return mapping[bopomofo][0];
        } else {
            // temporary fallback
            return L'N';
        }
    }
    std::vector<char32_t>* lookup_all(const std::u16string& bopomofo) {
        if (mapping.contains(bopomofo)) {
            return &mapping[bopomofo];
        }
        return nullptr;
    }

    // 檢查是否存在某漢字
    bool contains(char32_t word) {
        return hanzi_set.contains(word);
    }

    const std::unordered_set<char32_t>& get_word_set() {
        return hanzi_set;
    }

private:
    static std::optional<std::filesystem::path> env_path(const wchar_t* name) {
        const DWORD required = GetEnvironmentVariableW(name, nullptr, 0);
        if (required == 0) {
            return std::nullopt;
        }

        std::wstring value(required, L'\0');
        const DWORD copied = GetEnvironmentVariableW(name, value.data(), required);
        if (copied == 0) {
            return std::nullopt;
        }

        value.resize(copied);
        return std::filesystem::path(value);
    }

    static std::optional<std::filesystem::path> dll_dir() {
        if (!tsf::Globals::hinstance) {
            return std::nullopt;
        }

        std::wstring buffer(MAX_PATH, L'\0');
        for (;;) {
            const DWORD copied =
                GetModuleFileNameW(tsf::Globals::hinstance, buffer.data(), static_cast<DWORD>(buffer.size()));
            if (copied == 0) {
                return std::nullopt;
            }

            if (copied < buffer.size() - 1) {
                buffer.resize(copied);
                return std::filesystem::path(buffer).parent_path();
            }

            buffer.resize(buffer.size() * 2);
        }
    }

    static std::optional<std::filesystem::path> first_existing_file(
        const std::vector<std::filesystem::path>& candidates) {
        for (const auto& candidate : candidates) {
            if (std::filesystem::is_regular_file(candidate)) {
                return candidate;
            }
        }
        return std::nullopt;
    }

    static std::filesystem::path resolve_mapping_file(std::source_location loc = std::source_location::current()) {
        std::vector<std::filesystem::path> candidates;

        if (auto tables_dir = env_path(L"LLAVON_IME_TABLES_DIR")) {
            candidates.push_back(*tables_dir / "bopomofo_char.json");
        }

        if (auto module_dir = dll_dir()) {
            candidates.push_back(module_dir->parent_path() / "tables" / "bopomofo_char.json");
        }

        std::filesystem::path this_file = std::filesystem::path(loc.file_name()).lexically_normal();
        if (this_file.is_relative()) {
            this_file = std::filesystem::absolute(this_file).lexically_normal();
        }

        const std::filesystem::path project_root = this_file.parent_path().parent_path().parent_path().parent_path();
        candidates.push_back(project_root / "table" / "bopomofo_char.json");
        candidates.push_back(project_root / "tables" / "bopomofo_char.json");

        if (const auto mapping_file = first_existing_file(candidates)) {
            return *mapping_file;
        }

        std::string message = "Mapping file not found. Checked:";
        for (const auto& candidate : candidates) {
            message += "\n  " + candidate.string();
        }
        throw std::runtime_error(message);
    }

    HanziMapEngine() {
        std::filesystem::path mapping_file = resolve_mapping_file();
        std::ifstream ifs(mapping_file);
        jsoncons::json j = jsoncons::json::parse(ifs);
        auto temp = j.as<std::unordered_map<std::string, std::vector<std::string>>>();
        for (auto& [k, v] : temp) {
            auto key = utf8::utf8to16(k);
            std::vector<char32_t> wvec;
            for (const auto& item : v) {
                char32_t hanzi = utf8::utf8to32(item)[0];
                wvec.push_back(hanzi);
                hanzi_set.insert(hanzi);
            }
            mapping[key] = std::move(wvec);
        }
    }
};

struct BopomofoPos {
    char16_t initial = 0, medial = 0, final = 0;
    char16_t tone = 0;
    int choose_index = 0;
    bool chosen = false;     // user 手動選擇
    bool predicted = false;  // engine 預測
    bool compositable = false;
    bool invalid = false;
    std::vector<char32_t> candidates;

    // TODO temp
    std::optional<std::vector<char32_t>> predicted_candidate;
    bool accept(char16_t c) {
        if (is_compositable() || invalid) {
            return false;
        }
        if (Bopomofo::initial.contains(c)) {
            initial = c;
        } else if (Bopomofo::medial.contains(c)) {
            medial = c;
        } else if (Bopomofo::final.contains(c)) {
            final = c;
        } else if (Bopomofo::tone.contains(c)) {
            tone = c;
            std::vector<char32_t>* candi = HanziMapEngine::instance().lookup_all(to_bopomofo_string());
            if (candi == nullptr) {
                invalid = true;
                return true;
            }
            compositable = true;
            candidates = *candi;
        }
        return true;
    }
    std::u16string current() const {
        if (!is_compositable()) {
            return to_bopomofo_string();
        }
        char32_t c = candidates.at(choose_index);
        std::u16string s;
        utf8::append16(c, s);
        return s;
    }
    char32_t current32() const {
        if (!is_compositable()) {
            throw std::logic_error("not compositable");
        }
        char32_t c = candidates.at(choose_index);
        return c;
    }

public:
    std::u16string to_bopomofo_string() const {
        std::u16string res;
        if (initial) res.push_back(initial);
        if (medial) res.push_back(medial);
        if (final) res.push_back(final);
        if (tone) res.push_back(tone);
        return res;
    }
    bool is_null() const {
        return initial == 0 && medial == 0 && final == 0 && tone == 0;
    }
    bool is_compositable() const {
        return compositable;
    }
    bool is_predictable_by_engine() const {
        return chosen || compositable;
    }
    bool is_invalid() const {
        return invalid;
    }
    void set_chosen_candidate(char32_t ch) {
        initial = 0;
        medial = 0;
        final = 0;
        tone = 0;
        choose_index = 0;
        chosen = true;
        predicted = true;
        compositable = true;
        invalid = false;
        predicted_candidate.reset();
        candidates = {ch};
    }
    void set_choose_index(int idx) {
        choose_index = idx;
        chosen = true;
        predicted = true;
    }
    const std::vector<char32_t>& get_candidates() const {
        if (!is_compositable()) {
            throw std::runtime_error("No candidates available");
        }
        return candidates;
    }
    const void set_candaiates(const std::vector<char32_t>& nc) {
        candidates = nc;
    }

    bool pop_last_bopomofo() {
        compositable = false;
        invalid = false;
        chosen = false;
        predicted = false;
        predicted_candidate.reset();
        candidates.clear();

        if (tone) { tone = 0; return true; }
        if (final) { final = 0; return true; }
        if (medial) { medial = 0; return true; }
        if (initial) { initial = 0; return true; }
        return false;
    }
};

}  // namespace tsf
