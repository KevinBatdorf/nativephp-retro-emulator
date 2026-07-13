#include "sfc_pak.hpp"
#include <algorithm>
#include <cstring>

// Header scoring (mirrors mia's SuperFamicom::scoreHeader logic)
// base is the offset of the 64-byte header block in the ROM image.
uint32_t SfcPakBuilder::scoreHeader(const uint8_t* rom, size_t size, uint32_t base) {
    int score = 0;
    // Need at least base + 0x50 bytes to read reset vector.
    if (size < base + 0x50) return 0;

    const uint8_t* h = rom + base;

    uint8_t  mapMode    = h[0x25] & ~0x10u;         // ignore FastROM bit
    uint16_t complement = uint16_t(h[0x2c]) | (uint16_t(h[0x2d]) << 8);
    uint16_t checksum   = uint16_t(h[0x2e]) | (uint16_t(h[0x2f]) << 8);
    uint16_t resetVec   = uint16_t(h[0x4c]) | (uint16_t(h[0x4d]) << 8);

    // Reset vector below $8000 → not a valid ROM header.
    if (resetVec < 0x8000) return 0;

    // First opcode at the reset vector address (mapped within the current bank).
    uint32_t opcodeOffset = (base & ~0x7fffu) | (resetVec & 0x7fffu);
    if (opcodeOffset >= size) return 0;
    uint8_t opcode = rom[opcodeOffset];

    if (opcode == 0x78 || opcode == 0x18 || opcode == 0x38 ||
        opcode == 0x9c || opcode == 0x4c || opcode == 0x5c) score += 8;
    else if (opcode == 0xc2 || opcode == 0xe2 || opcode == 0xad ||
             opcode == 0xae || opcode == 0xac || opcode == 0xaf ||
             opcode == 0xa9 || opcode == 0xa2 || opcode == 0xa0 ||
             opcode == 0x20 || opcode == 0x22) score += 4;
    else if (opcode == 0x40 || opcode == 0x60 || opcode == 0x6b ||
             opcode == 0xcd || opcode == 0xec || opcode == 0xcc) score -= 4;
    else if (opcode == 0x00 || opcode == 0x02 || opcode == 0xdb ||
             opcode == 0x42 || opcode == 0xff)                   score -= 8;

    if (uint16_t(checksum + complement) == 0xffffu) score += 4;

    // Map-mode sanity check.
    if (base == 0x7fb0 && mapMode == 0x20) score += 2;
    if (base == 0xffb0 && mapMode == 0x21) score += 2;

    return (uint32_t)std::max(0, score);
}

// Extract 21-char ASCII title from the header block (at offset 0x10).
std::string SfcPakBuilder::extractTitle(const uint8_t* header) {
    std::string title;
    for (int i = 0; i < 21; i++) {
        uint8_t c = header[0x10 + i];
        if (c == 0x00) break;
        if (c >= 0x20 && c <= 0x7e) title += char(c);
    }
    while (!title.empty() && title.back() == ' ') title.pop_back();
    return title;
}

SfcPakBuilder::CartridgeInfo SfcPakBuilder::detectHeader(const uint8_t* rom, size_t size) {
    CartridgeInfo info;
    if (!rom || size < 0x8000) return info;

    // Strip 512-byte copier header if present.
    const uint8_t* data = rom;
    size_t dataSize = size;
    if ((size & 0x7fff) == 512) {
        data += 512;
        dataSize -= 512;
    }
    if (dataSize < 0x8000) return info;

    // Sufami Turbo base cartridge — the BIOS ROM carrying the two slots. mia
    // keys on the "BANDAI SFC-ADX" signature (super-famicom.cpp:570) and uses the
    // ST-LOROM board, whose two slot nodes make ares create the Sufami Turbo slot
    // ports. (Slot .st carts are built by makeSufamiSlotPak, not this path.)
    if (dataSize >= 14 && std::memcmp(data, "BANDAI SFC-ADX", 14) == 0) {
        info.board  = "ST-LOROM";
        info.title  = "Sufami Turbo";
        info.region = "NTSC";
        info.sramSize = 0;   // the ST-LOROM board defines any base RAM
        return info;
    }

    uint32_t loScore = scoreHeader(data, dataSize, 0x7fb0);
    uint32_t hiScore = scoreHeader(data, dataSize, 0xffb0);

    uint32_t headerBase = (loScore >= hiScore) ? 0x7fb0 : 0xffb0;
    bool isHiRom = (headerBase == 0xffb0);

    const uint8_t* h = data + headerBase;

    // BS-X (Satellaview) base cartridge — serial "ZBSJ" (super-famicom.cpp:587)
    // → the BS-MCC board, whose BS Memory slot makes ares create the "BS Memory
    // Slot" port. (BS .bs carts are built by makeBsMemoryPak.)
    if (std::memcmp(h + 0x02, "ZBSJ", 4) == 0) {
        info.board  = "BS-MCC-RAM";
        info.title  = "Satellaview BS-X";
        info.region = "NTSC";
        // BS-MCC-RAM maps two writable memories the base pak must allocate, or the
        // BIOS reads a null pointer and segfaults: a Save RAM (super-famicom.cpp:797
        // ramSize formula; falls back to the 0x8000 mapped by the board) and the
        // 512 KiB MCC download PSRAM (.bsx, super-famicom.cpp:334).
        uint8_t ramByte = h[0x28];
        info.sramSize = ramByte ? (1u << (((ramByte - 1) & 7) + 1)) << 10 : 0x8000u;
        info.downloadRamSize = 0x80000;
        return info;
    }

    info.title = extractTitle(h);
    if (info.title.empty()) info.title = "Unknown";

    uint8_t regionByte = h[0x29];
    info.region = (regionByte == 0x02 || (regionByte >= 0x03 && regionByte <= 0x0c))
                  ? "PAL" : "NTSC";

    // SRAM size (byte 0x28): encoded as 1 << (value - 1) KB, 0 = no SRAM.
    uint8_t sramByte = h[0x28];
    info.sramSize = sramByte ? (1u << ((sramByte - 1) & 7)) * 1024 : 0;

    bool hasRam = (info.sramSize > 0);
    if (isHiRom) {
        info.board = hasRam ? "HIROM-RAM" : "HIROM";
    } else {
        if (hasRam && dataSize <= 0x200000) {
            info.board = "LOROM-RAM";
        } else if (hasRam) {
            info.board = "LOROM-RAM#A";
        } else {
            info.board = "LOROM";
        }
    }

    return info;
}

std::shared_ptr<vfs::directory> SfcPakBuilder::makeSystemPak(
    const uint8_t* iplRom,    size_t iplSize,
    const uint8_t* boardsBml, size_t bmlSize)
{
    auto pak = std::make_shared<vfs::directory>();

    // ipl.rom: 64-byte SPC700 boot ROM. Provide zeros if not available;
    // the SMP will not boot correctly but the emulator will not crash.
    if (iplRom && iplSize > 0) {
        pak->append("ipl.rom", std::span<const u8>(iplRom, iplSize));
    } else {
        pak->append("ipl.rom", uint64_t(64));  // writable zeroed buffer
    }

    // boards.bml is required for memory-map lookup.
    if (boardsBml && bmlSize > 0) {
        pak->append("boards.bml", std::span<const u8>(boardsBml, bmlSize));
    }

    return pak;
}

std::shared_ptr<vfs::directory> SfcPakBuilder::makeCartridgePak(
    const CartridgeInfo& info,
    const uint8_t* rom, size_t romSize)
{
    auto pak = std::make_shared<vfs::directory>();

    // Strip 512-byte copier header if present.
    if (romSize > 0 && (romSize & 0x7fff) == 512) {
        rom += 512;
        romSize -= 512;
    }

    pak->setAttribute<nall::string>("title",  nall::string(info.title.c_str()));
    pak->setAttribute<nall::string>("board",  nall::string(info.board.c_str()));
    pak->setAttribute<nall::string>("region", nall::string(info.region.c_str()));

    pak->append("program.rom", std::span<const u8>(rom, romSize));

    if (info.sramSize > 0) {
        pak->append("save.ram", uint64_t(info.sramSize));
    }

    if (info.downloadRamSize > 0) {
        pak->append("download.ram", uint64_t(info.downloadRamSize));
    }

    return pak;
}

std::shared_ptr<vfs::directory> SfcPakBuilder::makeSufamiSlotPak(
    const uint8_t* rom, size_t romSize)
{
    auto pak = std::make_shared<vfs::directory>();

    if (romSize > 0 && (romSize & 0x7fff) == 512) {   // strip copier header
        rom += 512;
        romSize -= 512;
    }

    pak->append("program.rom", std::span<const u8>(rom, romSize));

    // Cart header: rom[0x37] is RAM size in 2 KiB units (mia SufamiTurbo::analyze).
    uint32_t ramSize = romSize > 0x37 ? uint32_t(rom[0x37]) * 2048u : 0u;
    if (ramSize > 0) {
        pak->append("save.ram", uint64_t(ramSize));
    }

    return pak;
}

std::shared_ptr<vfs::directory> SfcPakBuilder::makeBsMemoryPak(
    const uint8_t* rom, size_t romSize)
{
    auto pak = std::make_shared<vfs::directory>();

    if (romSize > 0 && (romSize & 0x7fff) == 512) {   // strip copier header
        rom += 512;
        romSize -= 512;
    }

    // BS Memory carts are flash cassettes — the "BS Memory Cartridge" node reads
    // program.flash (mia bs-memory.cpp defaults Flash). The bytes double as the
    // flash contents; a .sav persists writes.
    pak->append("program.flash", std::span<const u8>(rom, romSize));

    return pak;
}
