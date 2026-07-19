// Compiles mia's medium analyzers (unmodified, from the ares submodule)
// against the mia_shim.hpp stand-ins. See mia_shim.hpp for the full story.

#include "mia_shim.hpp"
#include "embedded_firmware.hpp"
#include "multi_pak.hpp"

namespace Resource::MegaDrive {
  const unsigned char* SVP = EmbeddedFirmware::MdSvp;
}

namespace mia {

// --- stub definitions — referenced by the mediums' load()/save() bodies,
// --- never called through the analyze-only flow.
auto Pak::read(string) -> std::vector<u8> { return {}; }
auto Pak::append(std::vector<u8>&, string) -> bool { return false; }
auto Pak::load(string, string, string) -> bool { return false; }
auto Pak::save(string, string, string) -> bool { return false; }
auto Pak::load(Markup::Node, string, string) -> bool { return false; }
auto Pak::save(Markup::Node, string, string) -> bool { return false; }

auto Medium::loadDatabase() -> bool { return true; }
auto Medium::manifestDatabase(string) -> string { return {}; }

// CompactDisc sector readers — mia/medium/medium.cpp:163-283 verbatim (BCD +
// CUE paths; the CHD path is compile-guarded out of our build). Called at
// disc load: PlayStation::analyze() reads the license sector for the region.
auto CompactDisc::manifestAudio(string location) -> string {
  string manifest;
  manifest += "game\n";
  manifest +={"  name:  ", Medium::name(location), "\n"};
  manifest +={"  title: ", Medium::name(location), "\n"};
  manifest +=   "  region: NTSC, NTSC-J, NTSC-U, PAL\n"; // We need a region to bootstrap
  manifest += "  audio\n";
  return manifest;
}

auto CompactDisc::isAudioCd(string pathname) -> bool {
  auto fp = file::open({pathname, "cd.rom"}, file::mode::read);
  if(!fp) return {};

  std::vector<u8> toc;
  toc.resize(96 * 7500);
  for(u32 sector : range(7500)) {
    fp.read({toc.data() + 96 * sector, 96});
  }
  CD::Session session;
  session.decode(toc, 96);

  //If the disc contains no data tracks, we are an audio cd
  for(u32 trackID : range(100)) {
    if(auto& track = session.tracks[trackID]) {
      if(track.isData()) return false;
    }
  }

  return true;
}

auto CompactDisc::readDataSector(string pathname, u32 sectorID) -> std::vector<u8> {
  if(pathname.iendsWith(".bcd")) {
    return readDataSectorBCD(pathname, sectorID);
  }
  if(pathname.iendsWith(".cue")) {
    return readDataSectorCUE(pathname, sectorID);
  }
  return {};
}

auto CompactDisc::readDataSectorBCD(string pathname, u32 sectorID) -> std::vector<u8> {
  auto fp = file::open({pathname, "cd.rom"}, file::mode::read);
  if(!fp) return {};

  std::vector<u8> toc;
  toc.resize(96 * 7500);
  for(u32 sector : range(7500)) {
    fp.read({toc.data() + 96 * sector, 96});
  }
  CD::Session session;
  session.decode(toc, 96);

  for(u32 trackID : range(100)) {
    if(auto& track = session.tracks[trackID]) {
      if(!track.isData()) continue;
      if(auto index = track.index(1)) {
        std::vector<u8> sector;
        sector.resize(2448);
        fp.seek(2448 * (abs(session.leadIn.lba) + index->lba + sectorID) + 16);
        fp.read({sector.data(), 2448});
        return sector;
      }
    }
  }

  return {};
}

auto CompactDisc::readDataSectorCUE(string filename, u32 sectorID) -> std::vector<u8> {
  Decode::CUE cuesheet;
  if(!cuesheet.load(filename, nullptr, nullptr)) return {};

  for(auto& file : cuesheet.files) {
    u64 offset = 0;
    auto location = string{Location::path(filename), file.name};

    if(file.type == "binary") {
      auto binary = file::open(location, nall::file::mode::read);
      if(!binary) continue;
      for(auto& track : file.tracks) {
        for(auto& index : track.indices) {
          u32 sectorSize = 0;
          if(track.type == "mode1/2048") sectorSize = 2048;
          if(track.type == "mode1/2352") sectorSize = 2352;
          if(track.type == "mode2/2352") sectorSize = 2352;
          if(sectorSize && index.number == 1) {
            binary.seek(offset + (sectorSize * sectorID) + (sectorSize == 2352 ? 16 : 0));
            std::vector<u8> sector;
            sector.resize(2048);
            binary.read({sector.data(), sector.size()});
            return sector;
          }
          offset += track.sectorSize() * index.sectorCount();
        }
      }
    }

    if(file.type == "wave") {
      Decode::WAV wave;
      if(!wave.open(location)) continue;
      offset += wave.headerSize;
      for(auto& track : file.tracks) {
        auto length = track.sectorSize();
        for(auto& index : track.indices) {
          offset += track.sectorSize() * index.sectorCount();
        }
      }
    }
  }

  return {};
}

#include <mia/medium/famicom.cpp>
#include <mia/medium/game-boy.cpp>
#include <mia/medium/mega-drive.cpp>
#include <mia/medium/game-boy-advance.cpp>
#include <mia/medium/playstation.cpp>

} // namespace mia

// Narrow entry points for multi_pak.cpp — the medium structs are local to
// this translation unit.
namespace MiaAnalyzers {

auto analyzeFamicom(std::vector<u8>& rom) -> string {
    mia::Famicom medium;
    return medium.analyze(rom);
}

auto analyzeGameBoy(std::vector<u8>& rom) -> string {
    mia::GameBoy medium;
    return medium.analyze(rom);
}

auto analyzeMegaDrive(std::vector<u8>& rom) -> string {
    mia::MegaDrive medium;
    return medium.analyze(rom);
}

auto analyzeGameBoyAdvance(std::vector<u8>& rom) -> string {
    mia::GameBoyAdvance medium;
    return medium.analyze(rom);
}

// Disc load runs mia's real medium load() — unlike the cartridge analyzers it
// needs the file PATH (vfs::cdrom resolves the cue's BIN references relative
// to it) and returns the assembled pak directly.
auto loadPlayStationDisc(const std::string& location) -> MiaAnalyzers::DiscLoad {
    mia::PlayStation medium;
    auto result = medium.load(string{location.c_str()});
    DiscLoad out;
    if(result != successful || !medium.pak) {
        out.error = result == romNotFound ? "media not found" : "could not analyze disc";
        return out;
    }
    out.pak    = medium.pak;
    out.title  = std::string(medium.pak->attribute("title").data());
    out.region = std::string(medium.pak->attribute("region").data());
    return out;
}

} // namespace MiaAnalyzers
