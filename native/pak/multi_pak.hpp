// In-memory pak construction for the fc / gb / md ares cores.
//
// Header analysis is delegated to mia's medium analyzers (see mia_shim.hpp);
// this file mirrors each medium's load() body — pak attributes + ROM slicing
// per the BML manifest — without any disk I/O. System pak firmware comes from
// the generated embedded_firmware blobs.
//
// SFC stays on SfcPakBuilder (predates this file; converged later).
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

// Build the system pak for "fc", "gb", or "md" from embedded firmware.
// fc: empty pak — the Famicom needs no system files.
// gb: boot.rom (DMG boot ROM).
// md: tmss.rom (TMSS boot ROM; only read when the TMSS setting is enabled).
auto makeSystemPak(const std::string& systemId) -> std::shared_ptr<nall::vfs::directory>;

// Analyze a raw ROM image with mia's header analyzer for the given system and
// assemble the cartridge pak in memory. Save RAM/EEPROM are appended as
// zero-filled writable buffers sized per the manifest (no persistence yet).
auto makeCartridgePak(const std::string& systemId,
                      const uint8_t* rom, size_t romSize) -> CartridgeResult;

} // namespace MultiPak
