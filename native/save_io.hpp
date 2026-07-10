// Battery-save persistence — shared by the Android JNI layer and the iOS C API.
//
// The cartridge pak's writable entries (save.ram, save.eeprom, …) are the live
// battery-backed memory: boards read them at connect and ares' System::save()
// writes memory back into them. Persistence is therefore:
//   seed:  disk → pak, after the pak is built and before the cartridge connects
//   flush: root->save() (caller's job), then pak → disk
//
// Files live at "<savePrefix>.<pakName>", e.g. "/…/saves/alttp.save.ram".
#pragma once

#include <nall/nall.hpp>
#include <nall/vfs.hpp>

#include <memory>
#include <string>

namespace SaveIO {

// Copy any existing save files under savePrefix into the pak's writable
// entries. Short files fill the head of the buffer; oversized files are
// truncated to the entry size. No-op for entries with no file on disk.
auto seed(const std::shared_ptr<nall::vfs::directory>& pak,
          const std::string& savePrefix) -> void;

// Write the pak's battery-backed entries to disk. The caller must run the
// system's save() first (root->save()) so board memory is current in the pak.
// Returns false if any file failed to write.
auto flush(const std::shared_ptr<nall::vfs::directory>& pak,
           const std::string& savePrefix) -> bool;

} // namespace SaveIO
