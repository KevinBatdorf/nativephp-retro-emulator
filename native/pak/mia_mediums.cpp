// Compiles mia's medium analyzers (unmodified, from the ares submodule)
// against the mia_shim.hpp stand-ins. See mia_shim.hpp for the full story.

#include "mia_shim.hpp"
#include "embedded_firmware.hpp"

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

#include <mia/medium/famicom.cpp>
#include <mia/medium/game-boy.cpp>
#include <mia/medium/mega-drive.cpp>
#include <mia/medium/sg-1000.cpp>
#include <mia/medium/master-system.cpp>
#include <mia/medium/atari-2600.cpp>
#include <mia/medium/game-boy-advance.cpp>
#include <mia/medium/neo-geo-pocket.cpp>
#include <mia/medium/msx.cpp>
#include <mia/medium/pc-engine.cpp>
#include <mia/medium/wonderswan.cpp>
#include <mia/medium/wonderswan-color.cpp>

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

auto analyzeSG1000(std::vector<u8>& rom) -> string {
    mia::SG1000 medium;
    return medium.analyze(rom);
}

auto analyzeMasterSystem(std::vector<u8>& rom) -> string {
    mia::MasterSystem medium;
    return medium.analyze(rom);
}

auto analyzeAtari2600(std::vector<u8>& rom) -> string {
    mia::Atari2600 medium;
    return medium.analyze(rom);
}

auto analyzeGameBoyAdvance(std::vector<u8>& rom) -> string {
    mia::GameBoyAdvance medium;
    return medium.analyze(rom);
}

auto analyzeNeoGeoPocket(std::vector<u8>& rom) -> string {
    mia::NeoGeoPocket medium;
    return medium.analyze(rom);
}

auto analyzeMSX(std::vector<u8>& rom) -> string {
    mia::MSX medium;
    return medium.analyze(rom);
}

auto analyzePCEngine(std::vector<u8>& rom) -> string {
    mia::PCEngine medium;
    return medium.analyze(rom);
}

auto analyzeWonderSwan(std::vector<u8>& rom) -> string {
    mia::WonderSwan medium;
    return medium.analyze(rom);
}

auto analyzeWonderSwanColor(std::vector<u8>& rom) -> string {
    mia::WonderSwanColor medium;
    return medium.analyze(rom);
}

} // namespace MiaAnalyzers
