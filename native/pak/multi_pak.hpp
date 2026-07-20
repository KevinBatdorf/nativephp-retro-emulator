// In-memory pak construction for every cartridge core except SFC.
//
// Header analysis is delegated to mia's medium analyzers (see mia_shim.hpp);
// this file mirrors each medium's load() body — pak attributes + ROM slicing
// per the BML manifest — without any disk I/O. System pak firmware comes from
// the generated embedded_firmware blobs.
//
// SFC stays on SfcPakBuilder.
#pragma once

#include <nall/nall.hpp>
#include <nall/vfs.hpp>

#include <memory>
#include <string>

namespace MultiPak {

struct CartridgeResult {
    std::shared_ptr<nall::vfs::directory> pak;
    std::string title;
    std::string region;   // raw manifest region string, e.g. "NTSC-J, NTSC-U"
    std::string error;    // non-empty on failure

    explicit operator bool() const { return (bool)pak; }
};

// Build the system pak from embedded firmware: fc none, gb/gbc boot.rom,
// md tmss.rom. bios is the dev-supplied firmware (empty = none) — gba
// append it as bios.rom; the embedded-firmware systems ignore it.
auto makeSystemPak(const std::string& systemId,
                   const std::vector<u8>& bios = {}) -> std::shared_ptr<nall::vfs::directory>;

// Analyze a raw ROM image with mia's header analyzer for the given system and
// assemble the cartridge pak in memory. Save RAM/EEPROM are appended as
// zero-filled writable buffers sized per the manifest (no persistence yet).
auto makeCartridgePak(const std::string& systemId,
                      const uint8_t* rom, size_t romSize) -> CartridgeResult;

} // namespace MultiPak
