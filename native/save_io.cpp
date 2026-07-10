#include "save_io.hpp"

#include <cstdio>
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

static auto readFile(const std::string& path) -> std::vector<uint8_t> {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return {};
    std::fseek(f, 0, SEEK_END);
    long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (size <= 0) { std::fclose(f); return {}; }
    std::vector<uint8_t> data((size_t)size);
    std::fread(data.data(), 1, (size_t)size, f);
    std::fclose(f);
    return data;
}

static auto writeFile(const std::string& path,
                      const uint8_t* data, size_t size) -> bool {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    size_t written = std::fwrite(data, 1, size, f);
    std::fclose(f);
    return written == size;
}

auto seed(const std::shared_ptr<nall::vfs::directory>& pak,
          const std::string& savePrefix) -> void {
    if (!pak || savePrefix.empty()) return;
    for (auto* name : kSaveNames) {
        auto fp = pak->write(name);
        if (!fp) continue;
        auto data = readFile(savePrefix + "." + name);
        if (data.empty()) continue;
        std::memcpy(fp->data(), data.data(),
                    std::min((size_t)fp->size(), data.size()));
    }
}

auto flush(const std::shared_ptr<nall::vfs::directory>& pak,
           const std::string& savePrefix) -> bool {
    if (!pak || savePrefix.empty()) return false;
    bool ok = true;
    for (auto* name : kSaveNames) {
        auto fp = pak->read(name);
        if (!fp || fp->size() == 0) continue;
        if (!writeFile(savePrefix + "." + name, fp->data(), (size_t)fp->size())) {
            ok = false;
        }
    }
    return ok;
}

} // namespace SaveIO
