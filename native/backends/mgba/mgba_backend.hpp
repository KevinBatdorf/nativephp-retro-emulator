// mGBA behind the seam — the bundled fast engine for gba (MPL-2.0; the
// plan's biggest measured gap: ares spends ~58% of a Thor core where
// dedicated GBA emulators spend single digits). One mCore per boot; mGBA's
// buffer-based state and savedata APIs map directly onto the contract.
#pragma once

#include "host/backend.hpp"

extern "C" {
#include <mgba/core/core.h>
#include <mgba/core/interface.h>
#include <mgba-util/audio-buffer.h>
#include <mgba-util/audio-resampler.h>
}

#include <memory>
#include <string>
#include <vector>

class MgbaBackend final : public EmuHost::Backend {
public:
    MgbaBackend() = default;
    ~MgbaBackend() override;

    const char* name() const override { return "mgba"; }
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
    // mGBA rumble peripheral — must be the FIRST member of this wrapper so
    // the engine's mRumble* casts back to the wrapper (C-style inheritance).
    struct Rumble {
        mRumble iface;
        MgbaBackend* self;
    };
    static void rumbleReset(mRumble* rumble, bool enable);
    static void rumbleSet(mRumble* rumble, bool enable, uint32_t sinceLast);
    static void rumbleIntegrate(mRumble* rumble, uint32_t period);

    void teardown();
    void drainAudio();

    mCore* core_ = nullptr;
    bool loaded_ = false;
    bool introDone_ = false;  // see inBootIntro

    std::vector<uint32_t> pixels_;     // engine XBGR frame, swizzled on push
    std::vector<uint32_t> converted_;
    unsigned width_  = 0;
    unsigned height_ = 0;

    mAudioBuffer resampled_ {};        // 48 kHz output of the resampler
    mAudioResampler resampler_ {};
    bool audioInit_ = false;
    std::vector<int16_t> drain_;

    // Cheats as per-frame RAM writes (classic action-replay semantics) —
    // applied after every visible frame from this mirror of the host table.
    std::vector<std::pair<uint32_t, uint8_t>> cheatWrites_;

    Rumble rumble_ {};
    EmuHost::HostPort* host_ = nullptr;
    int keyHandles_[10] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
};
