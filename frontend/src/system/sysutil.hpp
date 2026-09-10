#pragma once

#include "globals.h"

namespace tsf {
struct module_lock_updater {
    module_lock_updater() {
        ++tsf::Globals::dll_ref_count;
    }
    ~module_lock_updater() {
        --tsf::Globals::dll_ref_count;
    }
};
}  // namespace tsf