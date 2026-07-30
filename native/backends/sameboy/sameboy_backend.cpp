#include "sameboy_backend.hpp"

#include "host/backend_registry.hpp"
#include "host/host_log.hpp"
#include "host/system_catalog.hpp"

#include "embedded_firmware.hpp"

#include <cstring>

namespace {

// GB_key_t order (joypad.h): Right, Left, Up, Down, A, B, Select, Start.
// Names must match the catalog's gb/gbc button set so buttonHandle resolves.
const char* const kKeyNames[GB_KEY_MAX] = {
    "Right", "Left", "Up", "Down", "A", "B", "Select", "Start",
};

// The window the catalog advertises (0xC000, 0x2000 bytes) is the DMG-visible
// WRAM view; GB_DIRECT_ACCESS_RAM hands back the same first 8 KB the ares
// core exposed, so both engines answer identical addresses identically.
constexpr uint32_t kWramWindow = 0x2000;

} // namespace

SameBoyBackend::~SameBoyBackend() {
    if (gb_) {
        GB_free(gb_);
        GB_dealloc(gb_);
        gb_ = nullptr;
    }
}

const char* SameBoyBackend::version() const {
    return GB_VERSION;
}

std::vector<std::string> SameBoyBackend::systems() const {
    return {"gb", "gbc"};
}

EmuHost::Capabilities SameBoyBackend::capabilities(const std::string& systemId) const {
    EmuHost::Capabilities caps;
    if (systemId != "gb" && systemId != "gbc") return caps;

    caps.serialize    = true;
    caps.cheats       = true;
    caps.memoryAccess = true;
    caps.rumble       = true;   // MBC5 rumble carts
    caps.rateControl  = true;
    // No luminance/saturation/gamma/colorBleed door in SameBoy — the host
    // rejects setVideo loudly instead of pretending.
    caps.videoSettings = false;
    caps.options.push_back({"colorEmulation",
                            EmuHost::OptionInfo::Stage::Runtime,
                            EmuHost::OptionInfo::Kind::Boolean, {}});
    return caps;
}

EmuHost::Analysis SameBoyBackend::analyze(const std::string& systemId,
                                          const uint8_t* rom, size_t size) {
    EmuHost::Analysis analysis;
    if (systemId != "gb" && systemId != "gbc") {
        analysis.error = "unsupported system: " + systemId;
        return analysis;
    }
    // 0x150 covers the cartridge header; anything shorter cannot boot.
    if (size < 0x150) {
        analysis.error = "ROM too small for a Game Boy cartridge header";
        return analysis;
    }
    // Title bytes 0x134-0x143, ASCII, zero-padded (shortened on CGB carts).
    std::string title;
    for (size_t i = 0x134; i < 0x144 && i < size; i++) {
        char c = (char)rom[i];
        if (c == 0) break;
        if (c >= 0x20 && c < 0x7F) title += c;
    }
    analysis.ok    = true;
    analysis.title = title;
    // Region-free handheld: no region CSV, like the ares analyzer reported.
    analysis.token = std::make_shared<std::vector<uint8_t>>(rom, rom + size);
    return analysis;
}

void SameBoyBackend::pushFrame() {
    if (!host_ || pixels_.empty()) return;
    EmuHost::FrameGeometry geometry;
    geometry.width  = (double)width_;
    geometry.height = (double)height_;
    host_->pushFrame(pixels_.data(), width_, height_, width_, geometry);
}

void SameBoyBackend::onVblank(GB_gameboy_t* gb, GB_vblank_type_t) {
    auto* self = (SameBoyBackend*)GB_get_user_data(gb);
    if (self && !self->hidden_) self->pushFrame();
}

void SameBoyBackend::onSample(GB_gameboy_t* gb, GB_sample_t* sample) {
    auto* self = (SameBoyBackend*)GB_get_user_data(gb);
    if (!self || self->hidden_ || !self->host_) return;
    self->host_->pushAudioFrame(sample->left / 32768.0, sample->right / 32768.0);
}

uint32_t SameBoyBackend::encodeRgb(GB_gameboy_t*, uint8_t r, uint8_t g, uint8_t b) {
    // Same u32 layout the ares screens deliver (0xAARRGGBB — BGRA bytes
    // little-endian), so the Vulkan/Metal paths need no swizzle.
    return 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

void SameBoyBackend::onRumble(GB_gameboy_t* gb, double amplitude) {
    auto* self = (SameBoyBackend*)GB_get_user_data(gb);
    if (!self || !self->host_) return;
    // SameBoy reports one amplitude; the host contract carries strong/weak.
    auto level = (uint16_t)(amplitude * 0xFFFF);
    self->host_->setRumble(level, level);
}

EmuHost::BootResult SameBoyBackend::boot(const EmuHost::BootSpec& spec,
                                         EmuHost::HostPort& host,
                                         EmuHost::SaveMediaIO& saves) {
    EmuHost::BootResult result;
    auto rom = std::static_pointer_cast<std::vector<uint8_t>>(spec.token);
    if (!rom || rom->empty()) return result;

    host_ = &host;

    const bool cgb = spec.systemId == "gbc";
    gb_ = GB_init(GB_alloc(), cgb ? GB_MODEL_CGB_E : GB_MODEL_DMG_B);
    GB_set_user_data(gb_, this);

    if (cgb) {
        GB_load_boot_rom_from_buffer(gb_, EmbeddedFirmware::SameBoyCgbBoot,
                                     EmbeddedFirmware::SameBoyCgbBootSize);
    } else {
        GB_load_boot_rom_from_buffer(gb_, EmbeddedFirmware::SameBoyDmgBoot,
                                     EmbeddedFirmware::SameBoyDmgBootSize);
    }

    width_  = GB_get_screen_width(gb_);
    height_ = GB_get_screen_height(gb_);
    pixels_.assign((size_t)width_ * height_, 0);
    GB_set_pixels_output(gb_, pixels_.data());
    GB_set_rgb_encode_callback(gb_, encodeRgb);
    GB_set_vblank_callback(gb_, onVblank);

    GB_set_sample_rate(gb_, 48000);
    GB_apu_set_sample_callback(gb_, onSample);

    GB_set_rumble_mode(gb_, GB_RUMBLE_CARTRIDGE_ONLY);
    GB_set_rumble_callback(gb_, onRumble);

    GB_set_color_correction_mode(gb_, colorCorrection_
        ? GB_COLOR_CORRECTION_MODERN_BALANCED
        : GB_COLOR_CORRECTION_DISABLED);

    GB_load_rom_from_buffer(gb_, rom->data(), rom->size());

    // Seed battery memory before any frame runs, or the game reads blank SRAM.
    auto battery = saves.read("save.ram");
    if (!battery.empty()) {
        GB_load_battery_from_buffer(gb_, battery.data(), battery.size());
    }

    if (spec.bindings) bindPorts(*spec.bindings, host);

    loaded_ = true;
    host_->setRefreshRateHint(GB_get_usual_frame_rate(gb_));
    result.ok = true;
    return result;
}

void SameBoyBackend::unload(EmuHost::SaveMediaIO& saves) {
    if (gb_) {
        if (loaded_) collectSaveMedia(saves);
        GB_free(gb_);
        GB_dealloc(gb_);
        gb_ = nullptr;
    }
    loaded_ = false;
    pixels_.clear();
    for (auto& handle : keyHandles_) handle = -1;
}

void SameBoyBackend::bindPorts(const std::vector<EmuHost::PortBinding>& bindings,
                               EmuHost::HostPort& host) {
    host_ = &host;
    for (auto& handle : keyHandles_) handle = -1;
    // Built-in controls: the host hands one pseudo-binding on logical port 1.
    for (auto& binding : bindings) {
        if (binding.logical.empty()) continue;
        int port = binding.logical.front().logicalPort;
        for (int key = 0; key < GB_KEY_MAX; key++) {
            keyHandles_[key] = host.buttonHandle(port, kKeyNames[key]);
        }
        break;
    }
}

bool SameBoyBackend::tick(bool hidden) {
    if (!gb_ || !loaded_) return false;
    for (int key = 0; key < GB_KEY_MAX; key++) {
        if (keyHandles_[key] < 0) continue;
        GB_set_key_state(gb_, (GB_key_t)key, host_->sampleButton(keyHandles_[key]));
    }
    hidden_ = hidden;
    GB_run_frame(gb_);
    hidden_ = false;
    return true;
}

bool SameBoyBackend::serialize(std::vector<uint8_t>& out) {
    if (!gb_ || !loaded_) return false;
    out.resize(GB_get_save_state_size(gb_));
    GB_save_state_to_buffer(gb_, out.data());
    return true;
}

bool SameBoyBackend::unserialize(const uint8_t* data, size_t size) {
    if (!gb_ || !loaded_ || !data || size == 0) return false;
    return GB_load_state_from_buffer(gb_, data, size) == 0;
}

void SameBoyBackend::syncSave() {
    // GB_save_battery_to_buffer reads live cartridge RAM — nothing to sync.
}

bool SameBoyBackend::collectSaveMedia(EmuHost::SaveMediaIO& saves) {
    if (!gb_ || !loaded_) return false;
    int size = GB_save_battery_size(gb_);
    if (size <= 0) return false;
    std::vector<uint8_t> buffer((size_t)size);
    GB_save_battery_to_buffer(gb_, buffer.data(), buffer.size());
    // Same "save.ram" entry ares wrote: battery saves interchange between the
    // engines for non-RTC carts (SameBoy appends an RTC footer where the
    // cart has a clock; ares' seeding tolerates the extra bytes).
    return saves.write("save.ram", buffer.data(), buffer.size());
}

int SameBoyBackend::readMemory(uint32_t offset, uint8_t* out, uint32_t length) {
    if (!gb_ || !loaded_) return -1;
    size_t size = 0;
    uint16_t bank = 0;
    auto* wram = (const uint8_t*)GB_get_direct_access(gb_, GB_DIRECT_ACCESS_RAM, &size, &bank);
    if (!wram) return -1;
    size = std::min(size, (size_t)kWramWindow);
    if (offset + length > size) return -1;
    std::memcpy(out, wram + offset, length);
    return (int)length;
}

void SameBoyBackend::writeMemory(uint32_t offset, const uint8_t* data, uint32_t length) {
    if (!gb_ || !loaded_) return;
    size_t size = 0;
    uint16_t bank = 0;
    auto* wram = (uint8_t*)GB_get_direct_access(gb_, GB_DIRECT_ACCESS_RAM, &size, &bank);
    if (!wram) return;
    size = std::min(size, (size_t)kWramWindow);
    if (offset + length > size) return;
    std::memcpy(wram + offset, data, length);
}

void SameBoyBackend::setVideoSettings(float, float, float, bool, bool) {
    // capabilities().videoSettings is false — the host rejects before here.
}

void SameBoyBackend::setOverscan(bool) {
    // The Game Boy LCD has no overscan; nothing to trim or show.
}

bool SameBoyBackend::applyRuntimeToggle(const std::string& key, bool value) {
    if (key != "colorEmulation" || !gb_) return false;
    colorCorrection_ = value;
    GB_set_color_correction_mode(gb_, value
        ? GB_COLOR_CORRECTION_MODERN_BALANCED
        : GB_COLOR_CORRECTION_DISABLED);
    return true;
}

int SameBoyBackend::readRuntimeToggle(const std::string& key) {
    if (key != "colorEmulation" || !gb_) return -1;
    return colorCorrection_ ? 1 : 0;
}

std::string SameBoyBackend::readBootOption(const std::string&) {
    return "";   // SameBoy declares no boot options
}

bool SameBoyBackend::applyRateControl(double fillLevel) {
    if (!gb_ || !loaded_) return false;
    // Inverse of the ares stream skew: SameBoy's knob is the OUTPUT rate, so
    // an empty ring asks for a faster output (produce more per emulated
    // second). Same ±0.5% bound, unity at half full.
    constexpr double kMaxDelta = 0.005;
    double rate = 48000.0 * ((1.0 + kMaxDelta) - 2.0 * fillLevel * kMaxDelta);
    GB_set_sample_rate(gb_, (unsigned)rate);
    return true;
}

void SameBoyBackend::syncCheats(const std::unordered_map<uint32_t, uint32_t>& table) {
    if (!gb_) return;
    GB_remove_all_cheats(gb_);
    for (auto& [address, value] : table) {
        GB_add_cheat(gb_, "host", (uint16_t)address, GB_CHEAT_ANY_BANK,
                     (uint8_t)value, 0, false, true);
    }
}

static EmuHost::BackendRegistrar kRegistrar{
    "sameboy", [] { return std::make_unique<SameBoyBackend>(); }};

// Link anchor for static builds — named in ios/core_link.cpp's sum; a bare
// call-site read is elided at -O2 and the archive drops this object.
extern "C" int emu_backend_sameboy_link = 1;
