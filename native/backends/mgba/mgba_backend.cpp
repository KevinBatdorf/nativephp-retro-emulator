#include "mgba_backend.hpp"

#include "host/backend_registry.hpp"
#include "host/host_log.hpp"

extern "C" {
#include <mgba/core/version.h>
#include <mgba/gba/core.h>
#include <mgba/internal/arm/arm.h>
#include <mgba/internal/gba/input.h>
#include <mgba/internal/gba/memory.h>
#include <mgba-util/vfs.h>
}

#include <algorithm>
#include <cstring>

namespace {

// GBAKey order (input.h): A, B, Select, Start, Right, Left, Up, Down, R, L.
// Names must match the catalog's gba button set so buttonHandle resolves.
const char* const kKeyNames[10] = {
    "A", "B", "Select", "Start", "Right", "Left", "Up", "Down", "R", "L",
};

constexpr uint32_t kEwramSize = 0x40000;

// mGBA's 32-bit frames put red in the low byte; the host contract (and both
// platform renderers) put blue there.
inline uint32_t swizzleXbgr(uint32_t c) {
    return 0xFF000000u | ((c & 0xFFu) << 16) | (c & 0xFF00u) | ((c >> 16) & 0xFFu);
}

} // namespace

MgbaBackend::~MgbaBackend() {
    teardown();
}

const char* MgbaBackend::version() const {
    return projectVersion;
}

std::vector<std::string> MgbaBackend::systems() const {
    return {"gba"};
}

EmuHost::Capabilities MgbaBackend::capabilities(const std::string& systemId) const {
    EmuHost::Capabilities caps;
    if (systemId != "gba") return caps;

    caps.serialize    = true;
    caps.cheats       = true;   // per-frame RAM writes (action-replay semantics)
    caps.memoryAccess = true;
    caps.rumble       = true;   // GPIO rumble carts (WarioWare Twisted et al.)
    caps.rateControl  = true;
    caps.videoSettings = false;
    // No runtime toggles: mGBA has no color-emulation analog for GBA and the
    // wrapper's other toggles are ares node names.
    return caps;
}

EmuHost::Analysis MgbaBackend::analyze(const std::string& systemId,
                                       const uint8_t* rom, size_t size) {
    EmuHost::Analysis analysis;
    if (systemId != "gba") {
        analysis.error = "unsupported system: " + systemId;
        return analysis;
    }
    // 0xC0 covers the cartridge header (entry + logo + title + checksum).
    if (size < 0xC0) {
        analysis.error = "ROM too small for a GBA cartridge header";
        return analysis;
    }
    std::string title;
    for (size_t i = 0xA0; i < 0xAC && i < size; i++) {
        char c = (char)rom[i];
        if (c == 0) break;
        if (c >= 0x20 && c < 0x7F) title += c;
    }
    analysis.ok    = true;
    analysis.title = title;
    analysis.token = std::make_shared<std::vector<uint8_t>>(rom, rom + size);
    return analysis;
}

void MgbaBackend::rumbleReset(mRumble* rumble, bool enable) {
    auto* self = ((Rumble*)rumble)->self;
    if (self && self->host_ && !enable) self->host_->setRumble(0, 0);
}

void MgbaBackend::rumbleSet(mRumble* rumble, bool enable, uint32_t) {
    auto* self = ((Rumble*)rumble)->self;
    if (!self || !self->host_) return;
    auto level = (uint16_t)(enable ? 0xFFFF : 0);
    self->host_->setRumble(level, level);
}

void MgbaBackend::rumbleIntegrate(mRumble*, uint32_t) {}

EmuHost::BootResult MgbaBackend::boot(const EmuHost::BootSpec& spec,
                                      EmuHost::HostPort& host,
                                      EmuHost::SaveMediaIO& saves) {
    EmuHost::BootResult result;
    auto rom = std::static_pointer_cast<std::vector<uint8_t>>(spec.token);
    if (!rom || rom->empty()) return result;

    host_ = &host;

    core_ = GBACoreCreate();
    if (!core_ || !core_->init(core_)) {
        EMUHOST_LOGE("mgba: core init failed");
        teardown();
        return result;
    }
    // reset() reads config values; skipping this leaves the hash table as
    // garbage and the first lookup strlen-faults.
    mCoreInitConfig(core_, nullptr);

    bool bootAnim = false;
    if (spec.bootOptions) {
        auto it = spec.bootOptions->find("bootAnimation");
        if (it != spec.bootOptions->end()) bootAnim = (it->second == "true" || it->second == "1");
    }
    // mGBA's native BIOS skip: initialization runs, the animation does not.
    mCoreConfigSetDefaultIntValue(&core_->config, "skipBios", bootAnim ? 0 : 1);

    core_->baseVideoSize(core_, &width_, &height_);
    pixels_.assign((size_t)width_ * height_, 0);
    converted_.assign((size_t)width_ * height_, 0);
    core_->setVideoBuffer(core_, (mColor*)pixels_.data(), width_);

    // Audio: engine buffer at its native rate → 48 kHz through mGBA's own
    // resampler; DRC later skews the destination rate.
    core_->setAudioBufferSize(core_, 1024);
    mAudioBufferInit(&resampled_, 4096, 2);
    mAudioResamplerInit(&resampler_, mINTERPOLATOR_SINC);
    audioInit_ = true;

    rumble_.iface.reset = rumbleReset;
    rumble_.iface.setRumble = rumbleSet;
    rumble_.iface.integrate = rumbleIntegrate;
    rumble_.self = this;
    core_->setPeripheral(core_, mPERIPH_RUMBLE, &rumble_.iface);

    // A dev-supplied BIOS overrides mGBA's built-in HLE BIOS, mirroring the
    // biosPath contract ares' embedded open BIOS follows.
    if (spec.bios && !spec.bios->empty()) {
        if (VFile* bios = VFileMemChunk(spec.bios->data(), spec.bios->size())) {
            core_->loadBIOS(core_, bios, 0);
        }
    }

    VFile* romFile = VFileMemChunk(rom->data(), rom->size());
    if (!romFile || !core_->loadROM(core_, romFile)) {
        EMUHOST_LOGE("mgba: loadROM failed");
        teardown();
        return result;
    }

    auto battery = saves.read("save.ram");
    if (!battery.empty()) {
        core_->savedataRestore(core_, battery.data(), battery.size(), true);
    }

    core_->reset(core_);

    // The resampler's source must attach AFTER reset: reset() re-inits the
    // core's audio buffer, orphaning an earlier attachment.
    mAudioResamplerSetSource(&resampler_, core_->getAudioBuffer(core_),
                             core_->audioSampleRate(core_), true);
    mAudioResamplerSetDestination(&resampler_, &resampled_, 48000.0);

    if (spec.bindings) bindPorts(*spec.bindings, host);

    loaded_ = true;
    introDone_ = false;
    host_->setRefreshRateHint(59.7275);
    result.ok = true;
    return result;
}

void MgbaBackend::teardown() {
    if (audioInit_) {
        mAudioResamplerDeinit(&resampler_);
        mAudioBufferDeinit(&resampled_);
        audioInit_ = false;
    }
    if (core_) {
        mCoreConfigDeinit(&core_->config);
        core_->deinit(core_);
        core_ = nullptr;
    }
    loaded_ = false;
    pixels_.clear();
    converted_.clear();
    cheatWrites_.clear();
    for (auto& handle : keyHandles_) handle = -1;
}

void MgbaBackend::unload(EmuHost::SaveMediaIO& saves) {
    if (core_ && loaded_) collectSaveMedia(saves);
    teardown();
}

void MgbaBackend::bindPorts(const std::vector<EmuHost::PortBinding>& bindings,
                            EmuHost::HostPort& host) {
    host_ = &host;
    for (auto& handle : keyHandles_) handle = -1;
    for (auto& binding : bindings) {
        if (binding.logical.empty()) continue;
        int port = binding.logical.front().logicalPort;
        for (int key = 0; key < 10; key++) {
            keyHandles_[key] = host.buttonHandle(port, kKeyNames[key]);
        }
        break;
    }
}

void MgbaBackend::drainAudio() {
    // The GBA core rate is a live register: SOUNDBIAS writes retarget it at
    // any instant (gba/audio.c:232). Re-read it per pull, same buffer so the
    // resampler's phase survives — mirrors sdl-audio.c:131,142.
    mAudioResamplerSetSource(&resampler_, core_->getAudioBuffer(core_),
                             core_->audioSampleRate(core_), true);
    mAudioResamplerProcess(&resampler_);
    size_t available = mAudioBufferAvailable(&resampled_);
    while (available > 0) {
        size_t chunk = std::min(available, (size_t)512);
        drain_.resize(chunk * 2);
        size_t read = mAudioBufferRead(&resampled_, drain_.data(), chunk);
        for (size_t i = 0; i < read; i++) {
            host_->pushAudioFrame(drain_[i * 2] / 32768.0, drain_[i * 2 + 1] / 32768.0);
        }
        if (read == 0) break;
        available -= read;
    }
}

bool MgbaBackend::inBootIntro() {
    if (!loaded_ || !core_ || introDone_) return false;
    // Cartridge entry is 0x08000000; until the BIOS jumps there the animation
    // is still running. Latched because games SWI back into the BIOS forever.
    auto* cpu = (struct ARMCore*)core_->cpu;
    if ((uint32_t)cpu->gprs[15] >= 0x08000000) {
        introDone_ = true;
        return false;
    }
    return true;
}

bool MgbaBackend::tick(bool hidden) {
    if (!core_ || !loaded_) return false;

    uint32_t keys = 0;
    for (int key = 0; key < 10; key++) {
        if (keyHandles_[key] >= 0 && host_->sampleButton(keyHandles_[key])) {
            keys |= 1u << key;
        }
    }
    core_->setKeys(core_, keys);

    core_->runFrame(core_);

    if (!hidden) {
        for (auto& [address, value] : cheatWrites_) {
            core_->busWrite8(core_, address, value);
        }
        for (size_t i = 0; i < converted_.size(); i++) {
            converted_[i] = swizzleXbgr(pixels_[i]);
        }
        EmuHost::FrameGeometry geometry;
        geometry.width  = (double)width_;
        geometry.height = (double)height_;
        host_->pushFrame(converted_.data(), width_, height_, width_, geometry);
        drainAudio();
    } else {
        // Hidden run-ahead frame: advance state, discard output.
        mAudioResamplerProcess(&resampler_);
        mAudioBufferClear(&resampled_);
    }
    return true;
}

bool MgbaBackend::serialize(std::vector<uint8_t>& out) {
    if (!core_ || !loaded_) return false;
    out.resize(core_->stateSize(core_));
    return core_->saveState(core_, out.data());
}

bool MgbaBackend::unserialize(const uint8_t* data, size_t size) {
    if (!core_ || !loaded_ || !data || size < core_->stateSize(core_)) return false;
    return core_->loadState(core_, data);
}

void MgbaBackend::syncSave() {
    // savedataClone reads live savedata — nothing to sync first.
}

bool MgbaBackend::collectSaveMedia(EmuHost::SaveMediaIO& saves) {
    if (!core_ || !loaded_) return false;
    void* sram = nullptr;
    size_t size = core_->savedataClone(core_, &sram);
    if (!size || !sram) return false;
    bool ok = saves.write("save.ram", (const uint8_t*)sram, size);
    free(sram);
    return ok;
}

int MgbaBackend::readMemory(uint32_t offset, uint8_t* out, uint32_t length) {
    if (!core_ || !loaded_) return -1;
    size_t size = 0;
    auto* ewram = (const uint8_t*)core_->getMemoryBlock(core_, GBA_REGION_EWRAM, &size);
    if (!ewram) return -1;
    size = std::min(size, (size_t)kEwramSize);
    if (offset + length > size) return -1;
    std::memcpy(out, ewram + offset, length);
    return (int)length;
}

void MgbaBackend::writeMemory(uint32_t offset, const uint8_t* data, uint32_t length) {
    if (!core_ || !loaded_) return;
    size_t size = 0;
    auto* ewram = (uint8_t*)core_->getMemoryBlock(core_, GBA_REGION_EWRAM, &size);
    if (!ewram) return;
    size = std::min(size, (size_t)kEwramSize);
    if (offset + length > size) return;
    std::memcpy(ewram + offset, data, length);
}

void MgbaBackend::setVideoSettings(float, float, float, bool, bool) {
    // capabilities().videoSettings is false — the host rejects before here.
}

void MgbaBackend::setOverscan(bool) {
    // The GBA LCD has no overscan; nothing to trim or show.
}

bool MgbaBackend::applyRuntimeToggle(const std::string&, bool) {
    return false;
}

int MgbaBackend::readRuntimeToggle(const std::string&) {
    return -1;
}

std::string MgbaBackend::readBootOption(const std::string&) {
    return "";   // mGBA declares no boot options
}

bool MgbaBackend::applyRateControl(double) {
    // mAudioResamplerSetDestination has no phase compensation — re-calling
    // it with a jittering rate inserts a discontinuity at exactly the frame
    // rate (util/audio-resampler.c:61-68). Upstream only moves the rate when
    // fpsTarget moves; steady-state playback never re-calls it. Rate
    // mismatch lands on the host ring's drop-at-cap instead.
    return false;
}

void MgbaBackend::syncCheats(const std::unordered_map<uint32_t, uint32_t>& table) {
    cheatWrites_.clear();
    cheatWrites_.reserve(table.size());
    for (auto& [address, value] : table) {
        cheatWrites_.push_back({address, (uint8_t)value});
    }
}

static EmuHost::BackendRegistrar kRegistrar{
    "mgba", [] { return std::make_unique<MgbaBackend>(); }};

// Link anchor for static builds — named in ios/core_link.cpp's sum; a bare
// call-site read is elided at -O2 and the archive drops this object.
extern "C" int emu_backend_mgba_link = 1;
