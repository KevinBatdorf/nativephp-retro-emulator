// ares behind the seam: boot machinery, Platform callbacks, node caches,
// paks. Policy (latching, remaps, multitap numbering, rings, files) lives in
// EmulatorHost; anything here that looks like policy is only the translation
// of a resolved decision into node-tree terms.
#include "ares_backend.hpp"

#include "host/backend_registry.hpp"
#include "host/host_log.hpp"
#include "host/system_catalog.hpp"

#include "core_options.hpp"
#include "node_util.hpp"
#include "rate_control.hpp"
#include "save_io.hpp"

#include <algorithm>
#include <cstring>

#if defined(__ANDROID__)
#include <dlfcn.h>
#include <cstdio>
#endif

AresBackend::AresBackend() {
    // One engine, one Platform — the global hookup. The backend outlives
    // host init/destroy cycles; with no core running, no callback ever
    // fires, so an early hookup is inert.
    ares::platform = this;

#if defined(__ANDROID__)
    // The modular build ships each system as libares_core_<id>.so; dlopen
    // runs its SystemRegistry::Registrar, so loading IS registering and
    // systems() reflects exactly what the copy-assets hook bundled. The iOS
    // static build registers through link anchors instead (core_link.cpp).
    static const char* kCoreIds[] = {
        "fc", "sfc", "gb", "gba", "md",
    };
    for (auto* id : kCoreIds) {
        char name[64];
        std::snprintf(name, sizeof(name), "libares_core_%s.so", id);
        if (dlopen(name, RTLD_NOW | RTLD_LOCAL)) {
            EMUHOST_LOGI("ares core module loaded: %s", name);
        }
    }
#endif
}

AresBackend::~AresBackend() {
    if (ares::platform == this) ares::platform = nullptr;
}

const char* AresBackend::version() const {
    return ares::Version.data();
}

std::vector<std::string> AresBackend::systems() const {
    std::vector<std::string> ids;
    for (auto* def : SystemRegistry::all()) ids.push_back(def->id);
    return ids;
}

EmuHost::Capabilities AresBackend::capabilities(const std::string& systemId) const {
    EmuHost::Capabilities caps;
    auto* def = SystemRegistry::find(systemId);
    if (!def) return caps;

    caps.serialize     = true;
    caps.cheats        = true;
    caps.memoryAccess  = true;
    caps.rumble        = true;
    caps.rateControl   = true;
    caps.videoSettings = true;
    caps.slottedMedia  = def->makeSlotPak != nullptr;
    if (auto* sys = SystemCatalog::find(systemId)) {
        caps.multitap  = sys->multitapName != nullptr;
        caps.mouse     = sys->extraDevices.count("Mouse") > 0;
    }
    if (def->setOption) {
        caps.options.push_back({"Pixel Accuracy",
                                EmuHost::OptionInfo::Stage::Boot,
                                EmuHost::OptionInfo::Kind::Boolean, {}});
    }
    // Runtime toggles route through the CoreOptions scan; which nodes exist
    // depends on the booted core, so the list stays the wrapper's known keys.
    for (auto* key : {"colorEmulation", "deepBlackBoost", "interframeBlending", "showIcons"}) {
        caps.options.push_back({key,
                                EmuHost::OptionInfo::Stage::Runtime,
                                EmuHost::OptionInfo::Kind::Boolean, {}});
    }
    return caps;
}

EmuHost::Analysis AresBackend::analyze(const std::string& systemId,
                                       const uint8_t* rom, size_t size) {
    EmuHost::Analysis analysis;
    auto* def = SystemRegistry::find(systemId);
    if (!def) {
        analysis.error = "unsupported system: " + systemId;
        return analysis;
    }
    auto built = def->makeCartridgePak(rom, size);
    if (!built) {
        analysis.error = built.error;
        return analysis;
    }
    analysis.ok        = true;
    analysis.title     = built.title;
    analysis.regionCsv = built.region;
    analysis.token     = std::make_shared<SystemRegistry::CartridgePak>(std::move(built));
    return analysis;
}

void AresBackend::teardown() {
    if (root_) {
        // unload() joins worker threads (e.g. the video screen thread) before
        // the node tree is torn down — dropping root without it lets those
        // threads race into freed state and crash.
        root_->unload();
        root_.reset();
    }
    // Works around an upstream ares bug — a system loaded but never powered
    // strands dangling Thread entry points that wedge or corrupt a later
    // load. Full story on the declaration (system_registry.hpp).
    SystemRegistry::clearStaleEntryPoints();

    loaded_ = false;
    def_    = nullptr;
    systemPak_.reset();
    cartridgePak_.reset();
    slotPak_[0].reset();
    slotPak_[1].reset();
    buttonCache_.clear();
    axisCache_.clear();
    audioStreams_.clear();
}

EmuHost::BootResult AresBackend::boot(const EmuHost::BootSpec& spec,
                                      EmuHost::HostPort& host,
                                      EmuHost::SaveMediaIO& saves) {
    EmuHost::BootResult result;

    auto* def = SystemRegistry::find(spec.systemId);
    if (!def) return result;
    auto built = std::static_pointer_cast<SystemRegistry::CartridgePak>(spec.token);
    if (!built || !built->pak) return result;

    host_ = &host;
    cheatTable_ = host.cheatTable();

    auto loadName = SystemRegistry::loadNameFor(*def, spec.region);
    EMUHOST_LOGI("ares boot '%s'", loadName.c_str());

    // Boot options precede load(), as desktop does (super-famicom.cpp:123) —
    // SNES picks its PPU implementation here and load() crashes without one.
    if (def->setOption && spec.bootOptions) {
        for (auto& [name, value] : *spec.bootOptions) {
            def->setOption(name, value);
        }
    }

    def_ = def;   // Platform::pak resolves against def_ from load() onward
    systemPak_ = def->makeSystemPak(*def, spec.bios ? *spec.bios : std::vector<uint8_t>{});
    if (!def->load(root_, *def, loadName)) {
        EMUHOST_LOGE("ares load failed: %s", loadName.c_str());
        teardown();
        return result;
    }

    // Controllers are re-bound on every boot from the host's resolved
    // bindings (registrations persist host-side across loadRom).
    if (spec.bindings) bindPorts(*spec.bindings, host);

    // Desktop applies its overscan setting to every screen on load. Cores
    // consult screen->overscan() per frame, so it takes effect immediately.
    for (auto& screen : NodeUtil::findAll<ares::Node::Video::Screen>(root_)) {
        screen->setOverscan(spec.overscan);
    }

    cartridgePak_ = built->pak;

    // Seed battery saves from disk before the boards read the pak at connect.
    SaveIO::seed(cartridgePak_, saves);

    auto cartridgeSlot =
        NodeUtil::findByName<ares::Node::Port>(root_, "Cartridge Slot");
    if (!cartridgeSlot) {
        EMUHOST_LOGE("boot: no Cartridge Slot port found");
        teardown();
        return result;
    }
    ares::Node::Peripheral baseCartridge = cartridgeSlot->allocate();
    cartridgeSlot->connect();

    // Slotted media: connecting an ST-LOROM base created the Sufami Turbo
    // slot ports UNDER the base cartridge peripheral (not the root), so find
    // them there. Two shapes: SuFami Turbo (two slots) or BS-X (one BS
    // Memory slot); the staged slot 0 feeds the single BS slot. Connect
    // before power-on so the game is present at boot.
    bool isBsx = baseCartridge &&
        (bool)NodeUtil::findByName<ares::Node::Port>(baseCartridge, "BS Memory Slot");
    struct SlotDef { const char* port; bool flash; };
    SlotDef slots[2];
    int slotCount;
    if (isBsx) {
        slots[0] = {"BS Memory Slot", true};
        slotCount = 1;
    } else {
        slots[0] = {"Sufami Turbo Slot A", false};
        slots[1] = {"Sufami Turbo Slot B", false};
        slotCount = 2;
    }
    for (int i = 0; i < slotCount; i++) {
        const auto* staged = spec.slot[i];
        if (!staged || staged->empty()) continue;
        if (!def->makeSlotPak) {
            EMUHOST_LOGE("system '%s' has no slot pak builder", def->id.c_str());
            continue;
        }
        auto slot = baseCartridge
            ? NodeUtil::findByName<ares::Node::Port>(baseCartridge, slots[i].port)
            : ares::Node::Port();
        if (!slot) {
            EMUHOST_LOGE("slot port '%s' not found", slots[i].port);
            continue;
        }
        slotPak_[i] = def->makeSlotPak(i, slots[i].flash, staged->data(), staged->size());
        slot->allocate();
        slot->connect();
        result.slotConnected[i] = (bool)NodeUtil::connected(slot);
        result.slotConsumed[i]  = true;   // pak copied the bytes
        EMUHOST_LOGI("slot '%s' connected=%d (%zu bytes)",
                     slots[i].port, result.slotConnected[i] ? 1 : 0, staged->size());
    }

    root_->power(false);
    loaded_ = true;
    result.ok = true;
    return result;
}

void AresBackend::unload(EmuHost::SaveMediaIO& saves) {
    if (loaded_ && cartridgePak_) {
        SaveIO::flush(cartridgePak_, saves);
    }
    teardown();
}

void AresBackend::cacheDevice(const ares::Node::Object& parent, int logicalPort,
                              EmuHost::HostPort& host) {
    for (auto& btn : NodeUtil::findAll<ares::Node::Input::Button>(parent)) {
        int handle = host.buttonHandle(logicalPort, std::string((const char*)btn->name()));
        if (handle >= 0) buttonCache_.push_back({btn, handle});
    }
    for (auto& axis : NodeUtil::findAll<ares::Node::Input::Axis>(parent)) {
        int handle = host.axisHandle(logicalPort, std::string((const char*)axis->name()));
        if (handle >= 0) axisCache_.push_back({axis, handle});
    }
}

void AresBackend::bindPorts(const std::vector<EmuHost::PortBinding>& bindings,
                            EmuHost::HostPort& host) {
    buttonCache_.clear();
    axisCache_.clear();
    if (!root_ || !def_) return;
    host_ = &host;

    auto* sys = SystemCatalog::find(def_->id);

    for (auto& binding : bindings) {
        if (binding.physicalPort == 0) {
            // Built-in controls (Game Boy) — always cached on the system node.
            if (!binding.logical.empty()) {
                cacheDevice(root_, binding.logical.front().logicalPort, host);
            }
            continue;
        }

        auto portName = std::string("Controller Port ")
                      + std::to_string(binding.physicalPort);
        auto port = NodeUtil::findByName<ares::Node::Port>(root_, portName.c_str());

        if (binding.device.empty()) {
            if (port) port->disconnect();
            continue;
        }
        if (!port) continue;

        if (binding.multitap) {
            port->allocate(binding.device.c_str());
            port->connect();
            auto tap = NodeUtil::connected(port);   // the multitap peripheral
            int local = 1;
            for (auto& assignment : binding.logical) {
                auto sub = tap
                    ? NodeUtil::findByName<ares::Node::Port>(
                          tap, (std::string("Controller Port ") + std::to_string(local)).c_str())
                    : ares::Node::Port();
                local++;
                if (!sub) continue;
                sub->allocate("Gamepad");
                sub->connect();
                cacheDevice(sub, assignment.logicalPort, host);
            }
        } else {
            // Never allocate an unresolvable device name — ares would build
            // whatever peripheral happens to match, with no descriptor to
            // sample it through.
            SystemCatalog::DeviceDescriptor desc;
            if (sys && SystemCatalog::resolveDevice(*sys, binding.device, desc)) {
                port->allocate(binding.device.c_str());
                port->connect();
                if (!binding.logical.empty()) {
                    cacheDevice(port, binding.logical.front().logicalPort, host);
                }
            }
        }
    }
}

bool AresBackend::tick(bool hidden) {
    if (!root_ || !loaded_) return false;
    // Run-ahead's hidden frame: cores suppress AV while the flag is set.
    ares::setRunAhead(hidden);
    root_->run();
    return true;
}

bool AresBackend::serialize(std::vector<uint8_t>& out) {
    if (!root_ || !loaded_) return false;
    try {
        auto s = root_->serialize(false);
        out.assign(s.data(), s.data() + s.size());
        return true;
    } catch (...) {
        EMUHOST_LOGE("serialize: exception");
        return false;
    }
}

bool AresBackend::unserialize(const uint8_t* data, size_t size) {
    if (!root_ || !loaded_ || !data || size == 0) return false;
    try {
        nall::serializer s{data, (u32)size};
        return root_->unserialize(s);
    } catch (...) {
        EMUHOST_LOGE("unserialize: exception");
        return false;
    }
}

void AresBackend::syncSave() {
    // System::save() writes board memory back into the cartridge pak.
    if (root_ && loaded_) root_->save();
}

bool AresBackend::collectSaveMedia(EmuHost::SaveMediaIO& saves) {
    if (!loaded_ || !cartridgePak_) return false;
    return SaveIO::flush(cartridgePak_, saves);
}

int AresBackend::readMemory(uint32_t offset, uint8_t* out, uint32_t length) {
    if (!def_ || !loaded_) return -1;
    for (uint32_t i = 0; i < length; i++) out[i] = def_->memRead(offset + i);
    return (int)length;
}

void AresBackend::writeMemory(uint32_t offset, const uint8_t* data, uint32_t length) {
    if (!def_ || !loaded_) return;
    for (uint32_t i = 0; i < length; i++) def_->memWrite(offset + i, data[i]);
}

void AresBackend::setVideoSettings(float luminance, float saturation, float gamma,
                                   bool colorBleed, bool overscan) {
    if (!root_) return;
    for (auto& screen : NodeUtil::findAll<ares::Node::Video::Screen>(root_)) {
        screen->setLuminance((f64)luminance);
        screen->setSaturation((f64)saturation);
        screen->setGamma((f64)gamma);
        screen->setColorBleed(colorBleed);
        screen->setOverscan(overscan);
    }
}

void AresBackend::setOverscan(bool overscan) {
    if (!root_) return;
    for (auto& screen : NodeUtil::findAll<ares::Node::Video::Screen>(root_)) {
        screen->setOverscan(overscan);
    }
}

bool AresBackend::applyRuntimeToggle(const std::string& key, bool value) {
    if (!root_) return false;
    return CoreOptions::applyBoolean(root_, key, value);
}

int AresBackend::readRuntimeToggle(const std::string& key) {
    if (!root_) return -1;
    return CoreOptions::readBoolean(root_, key);
}

std::string AresBackend::readBootOption(const std::string& name) {
    if (!def_ || !def_->getOption) return "";
    return def_->getOption(name);
}

bool AresBackend::applyRateControl(double fillLevel) {
    if (audioStreams_.empty()) return false;
    RateControl::apply(audioStreams_, fillLevel);
    return true;
}

// --- ares::Platform ----------------------------------------------------------

auto AresBackend::status(string_view message) -> void {
    EMUHOST_LOGI("ares status: %.*s", (int)message.size(), message.data());
}

auto AresBackend::attach(ares::Node::Object node) -> void {
    if (auto stream = NodeUtil::as<ares::Node::Audio::Stream>(node)) {
        stream->setResamplerFrequency(48000.0);
        audioStreams_ = NodeUtil::findAll<ares::Node::Audio::Stream>(root_);
        EMUHOST_LOGI("audio stream attached — %zu total", audioStreams_.size());
    }
}

auto AresBackend::detach(ares::Node::Object node) -> void {
    if (auto stream = NodeUtil::as<ares::Node::Audio::Stream>(node)) {
        audioStreams_ = NodeUtil::findAll<ares::Node::Audio::Stream>(root_);
        std::erase(audioStreams_, stream);
        EMUHOST_LOGI("audio stream detached — %zu remaining", audioStreams_.size());
    }
}

auto AresBackend::pak(ares::Node::Object node) -> std::shared_ptr<vfs::directory> {
    if (!def_) return {};
    // The root IS the system node — match by identity, not by name.
    if (node == root_) return systemPak_;
    auto name = node->name();
    if (name == def_->cartridgeNode.c_str()) return cartridgePak_;
    // Sufami Turbo slot carts — same node name in both slots, so the parent
    // port ("Sufami Turbo Slot A" / "…B") picks which staged pak answers.
    if (name == "Sufami Turbo Cartridge") {
        auto parent = ares::Node::parent(node);
        std::string port = parent ? std::string((const char*)parent->name()) : "";
        if (port == "Sufami Turbo Slot A") return slotPak_[0];
        if (port == "Sufami Turbo Slot B") return slotPak_[1];
    }
    if (name == "BS Memory Cartridge") return slotPak_[0];
    return {};
}

auto AresBackend::video(ares::Node::Video::Screen node, const u32* data,
                        u32 pitch, u32 width, u32 height) -> void {
    if (!host_) return;
    // Runs on ares' screen worker thread — the host buffers under its frame
    // lock. NOTE: pitch is in BYTES (Screen::refresh passes width * 4).
    EmuHost::FrameGeometry geometry;
    geometry.width    = (double)node->width();
    geometry.height   = (double)node->height();
    geometry.scaleX   = node->scaleX();
    geometry.scaleY   = node->scaleY();
    geometry.aspectX  = node->aspectX();
    geometry.aspectY  = node->aspectY();
    geometry.rotation = node->rotation();
    host_->pushFrame(data, width, height, pitch / sizeof(u32), geometry);
}

auto AresBackend::audio(ares::Node::Audio::Stream) -> void {
    if (!host_ || audioStreams_.empty()) return;

    while (true) {
        // All streams must have at least one pending frame before we mix.
        for (auto& stream : audioStreams_) {
            if (!stream->pending()) return;
        }

        f64 samples[2] = {0.0, 0.0};
        for (auto& stream : audioStreams_) {
            f64 buf[2] = {0.0, 0.0};
            u32 channels = stream->read(buf);
            if (channels == 1) {
                samples[0] += buf[0];
                samples[1] += buf[0];
            } else {
                samples[0] += buf[0];
                samples[1] += buf[1];
            }
        }
        host_->pushAudioFrame(samples[0], samples[1]);
    }
}

auto AresBackend::input(ares::Node::Input::Input node) -> void {
    if (!host_) return;
    if (auto btn = NodeUtil::as<ares::Node::Input::Button>(node)) {
        for (auto& cached : buttonCache_) {
            if (cached.node == btn) {
                btn->setValue(host_->sampleButton(cached.handle));
                return;
            }
        }
        return;
    }
    if (auto axis = NodeUtil::as<ares::Node::Input::Axis>(node)) {
        for (auto& cached : axisCache_) {
            if (cached.node == axis) {
                axis->setValue(host_->consumeAxisDelta(cached.handle));
                return;
            }
        }
        axis->setValue(0);
        return;
    }
    if (auto rumble = NodeUtil::as<ares::Node::Input::Rumble>(node)) {
        host_->setRumble((uint16_t)rumble->strongValue(), (uint16_t)rumble->weakValue());
    }
}

// Consulted by the cores on every CPU bus read — the cached table pointer +
// empty() check keeps the no-cheat hot path to a single branch, exactly as
// the direct member access always did.
auto AresBackend::cheat(u32 address) -> maybe<u32> {
    if (!cheatTable_ || cheatTable_->empty()) return nothing;
    auto it = cheatTable_->find(address);
    if (it != cheatTable_->end()) return it->second;
    return nothing;
}

auto AresBackend::refreshRateHint(double refreshRate) -> void {
    if (host_) host_->setRefreshRateHint(refreshRate);
}

// Registers the backend at static-init time: dlopen (Android) or image load
// (iOS, via the link anchor below) makes ares available to the host.
static EmuHost::BackendRegistrar kRegistrar{
    "ares", [] { return std::make_unique<AresBackend>(); }};

// Link anchor for static builds — a self-registering TU is referenced by
// nothing, so archives would drop it; the iOS aggregator names this symbol
// to force the object (and its registrar) into the image.
extern "C" int emu_backend_ares_link = 1;
