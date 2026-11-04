#include "core/bopomofo.hpp"

#include <algorithm>
#include <cstdio>
#include <stdexcept>

int main() try {
    // The staged executable stands in for the frontend DLL. Resolve its actual
    // module path rather than injecting the repository's table directory.
    tsf::Globals::hinstance = GetModuleHandleW(nullptr);
    if (!tsf::Globals::hinstance) throw std::runtime_error("GetModuleHandleW failed");

    tsf::BopomofoPos syllable;
    for (const char16_t symbol : std::u16string_view{u"ㄋㄧˇ"}) {
        if (!syllable.accept(symbol)) throw std::runtime_error("reading was rejected");
    }
    if (!syllable.is_compositable() || !syllable.is_predictable_by_engine() ||
        std::ranges::find(syllable.candidates, U'你') == syllable.candidates.end()) {
        throw std::runtime_error("completed reading has no Hanzi candidates for backend conversion");
    }
    std::puts("PASS: module-relative table loading completes a reading and supplies Hanzi candidates");
    return 0;
} catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
}
