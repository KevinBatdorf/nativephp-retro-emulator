// In-memory pak construction for the fc / gb / md ares cores.
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

// Build the system pak for "fc", "gb", or "md" from embedded firmware.
// fc: empty pak — the Famicom needs no system files.
// gb: boot.rom (DMG boot ROM).
// md: tmss.rom (TMSS boot ROM; only read when the TMSS setting is enabled).
// bios: dev-supplied firmware image (empty = none) — the Master System
// appends it as bios.rom; embedded-firmware systems ignore it.
auto makeSystemPak(const std::string& systemId,
                   const std::vector<u8>& bios = {}) -> std::shared_ptr<nall::vfs::directory>;

// Analyze a raw ROM image with mia's header analyzer for the given system and
// assemble the cartridge pak in memory. Save RAM/EEPROM are appended as
// zero-filled writable buffers sized per the manifest (no persistence yet).
auto makeCartridgePak(const std::string& systemId,
                      const uint8_t* rom, size_t romSize) -> CartridgeResult;

// Build a media pak from a file PATH (disc systems: a .cue whose BIN
// references resolve relative to it, or a PS-X EXE). Runs mia's real medium
// load() — the pak carries manifest.bml + cd.rom (vfs::cdrom) / program.exe.
auto makeMediaPak(const std::string& systemId,
                  const std::string& path) -> CartridgeResult;

} // namespace MultiPak

// Shared contract with mia_mediums.cpp for disc loads.
namespace MiaAnalyzers {

struct DiscLoad {
    std::shared_ptr<nall::vfs::directory> pak;
    std::string title;
    std::string region;
    std::string error;   // non-empty on failure
};

auto loadPlayStationDisc(const std::string& location) -> DiscLoad;

} // namespace MiaAnalyzers
