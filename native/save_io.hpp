// Battery-save persistence — the ares side of the seam's SaveMediaIO door.
//
// The cartridge pak's writable entries (save.ram, save.eeprom, …) are the live
// battery-backed memory: boards read them at connect and ares' System::save()
// writes memory back into them. Persistence is therefore:
//   seed:  io → pak, after the pak is built and before the cartridge connects
//   flush: root->save() (caller's job), then pak → io
//
// The host's SaveMediaIO puts the bytes at "<savePrefix>.<pakName>"
// (e.g. "/…/saves/alttp.save.ram") — names must stay stable or existing
// user saves orphan.
#pragma once

#include <nall/nall.hpp>
#include <nall/vfs.hpp>

#include "host/backend.hpp"

#include <memory>

namespace SaveIO {

// Copy any existing save media into the pak's writable entries. Short media
// fills the head of the buffer; oversized media is truncated to the entry
// size. No-op for entries the io has nothing for.
auto seed(const std::shared_ptr<nall::vfs::directory>& pak,
          EmuHost::SaveMediaIO& io) -> void;

// Write the pak's battery-backed entries through the io. The caller must run
// the system's save() first (root->save()) so board memory is current in the
// pak. Returns false if any entry failed to write.
auto flush(const std::shared_ptr<nall::vfs::directory>& pak,
           EmuHost::SaveMediaIO& io) -> bool;

} // namespace SaveIO
