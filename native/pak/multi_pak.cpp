#include "multi_pak.hpp"
#include "embedded_firmware.hpp"

#include <vector>

using namespace nall;

// Narrow analyzer entry points exported by mia_mediums.cpp.
namespace MiaAnalyzers {
  auto analyzeFamicom(std::vector<u8>& rom) -> string;
  auto analyzeGameBoy(std::vector<u8>& rom) -> string;
  auto analyzeMegaDrive(std::vector<u8>& rom) -> string;
  auto analyzeGameBoyAdvance(std::vector<u8>& rom) -> string;
}

namespace MultiPak {

// Append a writable memory buffer named per mia's convention
// ([architecture.]content.type, downcased — "save.ram", "save.eeprom", …),
// sized per the manifest node and initialized to 0xFF like mia's
// Pak::load(Markup::Node…). Returns the vfs file for attribute stamping.
static auto appendMemory(std::shared_ptr<vfs::directory>& pak,
                         Markup::Node node) -> std::shared_ptr<vfs::file> {
    string name;
    if(auto architecture = node["architecture"].string()) name.append(architecture, ".");
    name.append(node["content"].string(), ".");
    name.append(node["type"].string());
    name.downcase();
    pak->append(name, node["size"].natural());
    auto fp = pak->write(name);
    if(fp) memory::fill<u8>(fp->data(), fp->size(), 0xff);
    return fp;
}

// --- Famicom — mirrors mia/medium/famicom.cpp load() ------------------------
static auto assembleFamicom(Markup::Node document, string& manifest,
                            std::vector<u8>& rom) -> std::shared_ptr<vfs::directory> {
    auto pak = std::make_shared<vfs::directory>();
    pak->setAttribute("title",       document["game/title"].string());
    pak->setAttribute("region",      document["game/region"].string());
    pak->setAttribute("board",       document["game/board"].string());
    pak->setAttribute("mirror",      document["game/board/mirror/mode"].string());
    pak->setAttribute("system",      document["game/system"].string());
    pak->setAttribute("chip",        document["game/board/chip/type"].string());
    pak->setAttribute("chip/key",    document["game/board/chip/key"].natural());
    pak->setAttribute("pinout/a0",   document["game/board/chip/pinout/a0"].natural());
    pak->setAttribute("pinout/a1",   document["game/board/chip/pinout/a1"].natural());
    pak->setAttribute("pinout/va10", document["game/board/pinout/va10"].natural());
    pak->append("manifest.bml", manifest);

    std::span<const u8> view{rom};
    if(auto node = document["game/board/memory(type=ROM,content=iNES)"]) {
        pak->append("ines.rom", {view.data(), node["size"].natural()});
        view = view.subspan(node["size"].natural());
    }
    if(auto node = document["game/board/memory(type=Flash,content=Program)"]) {
        pak->append("program.flash", {view.data(), node["size"].natural()});
        view = view.subspan(node["size"].natural());
    } else if(auto node = document["game/board/memory(type=ROM,content=Program)"]) {
        pak->append("program.rom", {view.data(), node["size"].natural()});
        view = view.subspan(node["size"].natural());
    }
    if(auto node = document["game/board/memory(type=ROM,content=Option)"]) {
        pak->append("option.rom", {view.data(), node["size"].natural()});
        view = view.subspan(node["size"].natural());
    }
    if(auto node = document["game/board/memory(type=ROM,content=Character)"]) {
        pak->append("character.rom", {view.data(), node["size"].natural()});
        view = view.subspan(node["size"].natural());
    }

    if(auto node = document["game/board/memory(type=RAM,content=Save)"])      appendMemory(pak, node);
    if(auto node = document["game/board/memory(type=EEPROM,content=Save)"])   appendMemory(pak, node);
    if(auto node = document["game/board/memory(type=RAM,content=Character)"]) appendMemory(pak, node);
    return pak;
}

// --- Game Boy — mirrors mia/medium/game-boy.cpp load() ----------------------
static auto assembleGameBoy(Markup::Node document, string& manifest,
                            std::vector<u8>& rom) -> std::shared_ptr<vfs::directory> {
    auto pak = std::make_shared<vfs::directory>();
    pak->setAttribute("title", document["game/title"].string());
    pak->setAttribute("board", document["game/board"].string());
    pak->append("manifest.bml", manifest);
    pak->append("program.rom", rom);

    if(auto node = document["game/board/memory(type=RAM,content=Save)"]) appendMemory(pak, node);
    if(auto node = document["game/board/memory(type=EEPROM,content=Save)"]) {
        if(auto fp = appendMemory(pak, node)) {
            fp->setAttribute("width", node["width"].natural());
        }
    }
    if(auto node = document["game/board/memory(type=Flash,content=Download)"]) appendMemory(pak, node);
    if(auto node = document["game/board/memory(type=RTC,content=Time)"])       appendMemory(pak, node);
    return pak;
}

// --- Mega Drive — mirrors mia/medium/mega-drive.cpp load() ------------------
static auto assembleMegaDrive(Markup::Node document, string& manifest,
                              std::vector<u8>& rom) -> std::shared_ptr<vfs::directory> {
    auto pak = std::make_shared<vfs::directory>();
    pak->setAttribute("title",    document["game/title"].string());
    pak->setAttribute("region",   document["game/region"].string());
    pak->setAttribute("board",    document["game/board"].string());
    pak->setAttribute("bootable", true);
    auto deviceList = document["game/device"].string();
    auto devices = nall::split(deviceList, ", ");
    pak->setAttribute("megacd",
        (bool)(std::ranges::find(devices, string{"Mega CD"}) != devices.end()));
    pak->append("manifest.bml", manifest);

    // Repair Virtua Racing dumps ripped without the SVP program ROM.
    if(document["game/board/memory(type=ROM,content=SVP)"]) {
        if(memory::compare(rom.data() + rom.size() - 0x800, EmbeddedFirmware::MdSvp, 0x800)) {
            rom.resize(rom.size() + 0x800);
            memory::copy(rom.data() + rom.size() - 0x800, EmbeddedFirmware::MdSvp, 0x800);
        }
    }

    std::span<const u8> view{rom};
    for(auto node : document.find("game/board/memory(type=ROM)")) {
        string name = {node["content"].string().downcase(), ".rom"};
        u32 size = node["size"].natural();
        if(view.size() < size) break;  //missing firmware
        pak->append(name, {view.data(), size});
        view = view.subspan(size);
    }

    if(auto node = document["game/board/memory(type=RAM,content=Save)"]) {
        if(auto fp = appendMemory(pak, node)) {
            fp->setAttribute("mode",    node["mode"].string());
            fp->setAttribute("address", node["address"].natural());
            fp->setAttribute("enable",  node["enable"].boolean());
        }
    }
    if(auto node = document["game/board/memory(type=EEPROM,content=Save)"]) {
        if(auto fp = appendMemory(pak, node)) {
            fp->setAttribute("address", node["address"].natural());
            fp->setAttribute("mode",    node["mode"].string());
            fp->setAttribute("rsda",    node["rsda"].natural());
            fp->setAttribute("wsda",    node["wsda"].natural());
            fp->setAttribute("wscl",    node["wscl"].natural());
        }
    }
    if(document["game/board(peripheral=J-Cart)"]) {
        pak->setAttribute("jcart", true);
    }
    return pak;
}

static auto assembleGameBoyAdvance(Markup::Node document, string& manifest,
                                   std::vector<u8>& rom) -> std::shared_ptr<vfs::directory> {
    auto pak = std::make_shared<vfs::directory>();
    pak->setAttribute("title", document["game/title"].string());
    pak->append("manifest.bml", manifest);
    pak->append("program.rom",  rom);
    if(auto node = document["game/board/memory(type=RAM,content=Save)"])    appendMemory(pak, node);
    if(auto node = document["game/board/memory(type=EEPROM,content=Save)"]) appendMemory(pak, node);
    if(auto node = document["game/board/memory(type=Flash,content=Save)"]) {
        if(auto fp = appendMemory(pak, node)) {
            fp->setAttribute("manufacturer", node["manufacturer"].string());
        }
    }
    if(auto node = document["game/board/memory(type=RTC,content=Time)"])    appendMemory(pak, node);
    return pak;
}

bool useFastGbBoot = false;

auto makeSystemPak(const std::string& systemId,
                   const std::vector<u8>& bios) -> std::shared_ptr<vfs::directory> {
    auto pak = std::make_shared<vfs::directory>();
    if(systemId == "gb") {
        if(useFastGbBoot) pak->append("boot.rom", std::span<const u8>(
            EmbeddedFirmware::SameBoyDmgBootFast, EmbeddedFirmware::SameBoyDmgBootFastSize));
        else pak->append("boot.rom", std::span<const u8>(
            EmbeddedFirmware::GbBootDmg1, EmbeddedFirmware::GbBootDmg1Size));
    } else if(systemId == "gbc") {
        if(useFastGbBoot) pak->append("boot.rom", std::span<const u8>(
            EmbeddedFirmware::SameBoyCgbBootFast, EmbeddedFirmware::SameBoyCgbBootFastSize));
        else pak->append("boot.rom", std::span<const u8>(
            EmbeddedFirmware::GbBootCgb1, EmbeddedFirmware::GbBootCgb1Size));
    } else if(systemId == "md") {
        pak->append("tmss.rom", std::span<const u8>(
            EmbeddedFirmware::MdTmss, EmbeddedFirmware::MdTmssSize));
    } else if(systemId == "gba") {
        // A dev-supplied BIOS (real dump, for accuracy) wins; otherwise fall back
        // to the embedded Cult-of-GBA open BIOS so gba boots with no setup.
        if(!bios.empty()) pak->append("bios.rom", bios);
        else pak->append("bios.rom", std::span<const u8>(
            EmbeddedFirmware::GbaBios, EmbeddedFirmware::GbaBiosSize));
    }
    // fc: no system files required.
    return pak;
}

auto makeCartridgePak(const std::string& systemId,
                      const uint8_t* rom, size_t romSize) -> CartridgeResult {
    CartridgeResult result;
    if(!rom || romSize == 0) {
        result.error = "empty ROM";
        return result;
    }
    std::vector<u8> data(rom, rom + romSize);

    string manifest;
    if(systemId == "fc")       manifest = MiaAnalyzers::analyzeFamicom(data);
    else if(systemId == "gb")  manifest = MiaAnalyzers::analyzeGameBoy(data);
    else if(systemId == "gbc") manifest = MiaAnalyzers::analyzeGameBoy(data);
    else if(systemId == "md")  manifest = MiaAnalyzers::analyzeMegaDrive(data);
    else if(systemId == "gba") manifest = MiaAnalyzers::analyzeGameBoyAdvance(data);
    else {
        result.error = "unknown system: " + systemId;
        return result;
    }

    auto document = BML::unserialize(manifest);
    if(!manifest || !document) {
        result.error = "could not analyze ROM header";
        return result;
    }

    if(systemId == "fc")       result.pak = assembleFamicom(document, manifest, data);
    else if(systemId == "gb")  result.pak = assembleGameBoy(document, manifest, data);
    else if(systemId == "gbc") result.pak = assembleGameBoy(document, manifest, data);
    else if(systemId == "md")  result.pak = assembleMegaDrive(document, manifest, data);
    else if(systemId == "gba") result.pak = assembleGameBoyAdvance(document, manifest, data);

    result.title  = std::string(document["game/title"].string().data());
    result.region = std::string(document["game/region"].string().data());
    return result;
}

} // namespace MultiPak

// The one instantiation every DSO imports — see native/cross_dso_vfs.hpp.
template std::shared_ptr<nall::vfs::file>
nall::vfs::directory::read<nall::vfs::file>(const nall::string&);
template std::shared_ptr<nall::vfs::file>
nall::vfs::directory::write<nall::vfs::file>(const nall::string&);
