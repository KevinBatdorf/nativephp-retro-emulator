#pragma once

#include <ares/ares.hpp>
#include <string>
#include <vector>
#include <cstdint>

// Minimal Super Famicom ROM header detector and pak builder.
// Covers the common LoROM/HiROM case without depending on the mia library or
// the boards.bml database for format detection.  The boards.bml is still
// provided in the system pak so ares can look up the memory map.
struct SfcPakBuilder {
    struct CartridgeInfo {
        std::string board;   // "LOROM", "HIROM", "LOROM-RAM", etc.
        std::string title;   // up to 21 chars from the ROM header
        std::string region;  // "NTSC" or "PAL"
        uint32_t    sramSize = 0; // in bytes, 0 if no SRAM
    };

    // Detect whether the ROM is LoROM or HiROM and extract basic header info.
    // Returns an empty CartridgeInfo (board == "") on unrecognisable input.
    static CartridgeInfo detectHeader(const uint8_t* rom, size_t size);

    // Build the system pak: ipl.rom + boards.bml.
    // iplRom may be null/empty (SMP will boot from zero — game hangs at audio
    // init, but the emulator does not crash; useful for no-audio tests).
    static std::shared_ptr<vfs::directory> makeSystemPak(
        const uint8_t* iplRom,    size_t iplSize,
        const uint8_t* boardsBml, size_t bmlSize);

    // Build the cartridge pak: program.rom + attributes + optional save.ram.
    static std::shared_ptr<vfs::directory> makeCartridgePak(
        const CartridgeInfo& info,
        const uint8_t* rom, size_t romSize);

    // Build a Sufami Turbo slot-cartridge pak: program.rom (the full .st) plus a
    // save.ram sized from the cart header (rom[0x37] = RAM in 2 KiB units). The
    // "Sufami Turbo Cartridge" node reads these when its slot connects.
    static std::shared_ptr<vfs::directory> makeSufamiSlotPak(
        const uint8_t* rom, size_t romSize);

private:
    static uint32_t scoreHeader(const uint8_t* rom, size_t size, uint32_t base);
    static std::string extractTitle(const uint8_t* header);
};
