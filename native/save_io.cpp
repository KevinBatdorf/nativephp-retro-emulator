#include "save_io.hpp"

#include <cstring>
#include <vector>

namespace SaveIO {

// Battery-backed pak entries across the compiled systems, named per mia's
// content.type convention (see mia/pak/pak.cpp).
static const char* kSaveNames[] = {
    "save.ram",
    "save.eeprom",
    "character.ram",
    "time.rtc",
    "download.flash",
};

auto seed(const std::shared_ptr<nall::vfs::directory>& pak,
          EmuHost::SaveMediaIO& io) -> void {
    if (!pak) return;
    for (auto* name : kSaveNames) {
        auto fp = pak->write(name);
        if (!fp) continue;
        auto data = io.read(name);
        if (data.empty()) continue;
        std::memcpy(fp->data(), data.data(),
                    std::min((size_t)fp->size(), data.size()));
    }
}

auto flush(const std::shared_ptr<nall::vfs::directory>& pak,
           EmuHost::SaveMediaIO& io) -> bool {
    if (!pak) return false;
    bool ok = true;
    for (auto* name : kSaveNames) {
        auto fp = pak->read(name);
        if (!fp || fp->size() == 0) continue;
        if (!io.write(name, fp->data(), (size_t)fp->size())) {
            ok = false;
        }
    }
    return ok;
}

} // namespace SaveIO
