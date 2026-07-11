// Parser for ares' raw cheat format: hex "ADDR:VALUE" pairs joined with '+'
// (desktop-ui tools/cheats.cpp semantics — malformed pairs are skipped,
// matching upstream). Shared by the Android JNI and iOS C API layers.
#pragma once

#include <cstdint>
#include <cstdlib>
#include <map>
#include <string>

namespace CheatParse {

inline std::map<uint32_t, uint32_t> parse(const std::string& code) {
    std::map<uint32_t, uint32_t> pairs;
    size_t start = 0;
    while (start <= code.size()) {
        size_t end = code.find('+', start);
        if (end == std::string::npos) end = code.size();
        std::string part = code.substr(start, end - start);
        size_t colon = part.find(':');
        if (colon != std::string::npos && colon > 0 && colon + 1 < part.size()) {
            std::string addrStr  = part.substr(0, colon);
            std::string valueStr = part.substr(colon + 1);
            char* addrEnd  = nullptr;
            char* valueEnd = nullptr;
            uint32_t addr  = (uint32_t)std::strtoul(addrStr.c_str(),  &addrEnd,  16);
            uint32_t value = (uint32_t)std::strtoul(valueStr.c_str(), &valueEnd, 16);
            if (addrEnd == addrStr.c_str() + addrStr.size() &&
                valueEnd == valueStr.c_str() + valueStr.size()) {
                pairs[addr] = value;
            }
        }
        start = end + 1;
    }
    return pairs;
}

}  // namespace CheatParse
