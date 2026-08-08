// SameBoy behind the seam — the bundled fast engine for gb/gbc
// (Expat/MIT; faster AND more accurate than ares' GB core on phones, see
// the measurements in the multicore plan). One GB_gameboy_t per boot;
// SameBoy is single-threaded and callback-driven, which maps 1:1 onto the
// host's tick model.
#pragma once

#include "host/backend.hpp"

extern "C" {
#include <Core/gb.h>
}

#include <memory>
#include <string>
#include <vector>

class SameBoyBackend final : public EmuHost::Backend {
public:
    SameBoyBackend() = default;
    ~SameBoyBackend() override;

    const char* name() const override { return "sameboy"; }
    const char* version() const override;
    std::vector<std::string> systems() const override;
    EmuHost::Capabilities capabilities(const std::string& systemId) const override;

    EmuHost::Analysis analyze(const std::string& systemId,
                              const uint8_t* rom, size_t size) override;
    EmuHost::BootResult boot(const EmuHost::BootSpec& spec,
                             EmuHost::HostPort& host,
                             EmuHost::SaveMediaIO& saves) override;
    void unload(EmuHost::SaveMediaIO& saves) override;
    void bindPorts(const std::vector<EmuHost::PortBinding>& bindings,
                   EmuHost::HostPort& host) override;

    bool tick(bool hidden) override;
    bool inBootIntro() override;
    bool serialize(std::vector<uint8_t>& out) override;
    bool unserialize(const uint8_t* data, size_t size) override;

    void syncSave() override;
    bool collectSaveMedia(EmuHost::SaveMediaIO& saves) override;

    int  readMemory(uint32_t offset, uint8_t* out, uint32_t length) override;
    void writeMemory(uint32_t offset, const uint8_t* data, uint32_t length) override;

    void setVideoSettings(float luminance, float saturation, float gamma,
                          bool colorBleed, bool overscan) override;
    void setOverscan(bool overscan) override;
    bool applyRuntimeToggle(const std::string& key, bool value) override;
    int  readRuntimeToggle(const std::string& key) override;
    std::string readBootOption(const std::string& name) override;
    bool applyRateControl(double fillLevel) override;
    void syncCheats(const std::unordered_map<uint32_t, uint32_t>& table) override;

private:
    // SameBoy C callbacks — trampoline through GB_set_user_data(this).
    static void     onVblank(GB_gameboy_t* gb, GB_vblank_type_t type);
    static void     onSample(GB_gameboy_t* gb, GB_sample_t* sample);
    static uint32_t encodeRgb(GB_gameboy_t* gb, uint8_t r, uint8_t g, uint8_t b);
    static void     onRumble(GB_gameboy_t* gb, double amplitude);

    void pushFrame();

    GB_gameboy_t* gb_ = nullptr;
    bool loaded_ = false;
    bool hidden_ = false;         // run-ahead hidden frame: suppress AV output

    bool colorCorrection_ = true; // the colorEmulation toggle's live value
    bool raw_ = false;   // rawAudio boot option; see capabilities()

    std::vector<uint32_t> pixels_;   // GB_set_pixels_output target, 160x144
    unsigned width_  = 0;
    unsigned height_ = 0;

    EmuHost::HostPort* host_ = nullptr;
    // Host input handles for the eight Game Boy keys, indexed by GB_key_t.
    int keyHandles_[GB_KEY_MAX] = {-1, -1, -1, -1, -1, -1, -1, -1};
};
