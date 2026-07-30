// The bring-your-own-core loader: any libretro core dropped into the app
// (resources/emulator-cores/, shipped by copy-assets) can serve a catalog
// system. The libretro ABI is the contract — a core self-reports its name,
// version, extensions and A/V timing at load; nothing here is core-specific.
//
// One adopted core at a time, matching the plugin's one-engine-per-process
// model — libretro callbacks are bare C function pointers with no user data,
// so the live instance is process-global by the API's own design.
#pragma once

#include "host/backend.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

struct retro_game_geometry;
struct retro_system_av_info;

class LibretroBackend final : public EmuHost::Backend {
public:
    LibretroBackend();
    ~LibretroBackend() override;

    // Post-adoption the identity is the core ("snes9x"): status responses
    // and save-state backend tags then name the engine actually serving.
    // While a game runs, the running core wins over a staged adoption so
    // state tags never claim the wrong engine. The registry is unaffected —
    // it matches on the registered entry name.
    const char* name() const override {
        if (loaded_) return current_.coreName.c_str();
        return bootTarget().handle ? bootTarget().coreName.c_str() : "libretro";
    }
    const char* version() const override;
    std::vector<std::string> systems() const override;
    EmuHost::Capabilities capabilities(const std::string& systemId) const override;
    bool adoptDynamicCore(const std::string& name, const std::string& systemId) override;

    EmuHost::Analysis analyze(const std::string& systemId,
                              const uint8_t* rom, size_t size) override;
    EmuHost::BootResult boot(const EmuHost::BootSpec& spec,
                             EmuHost::HostPort& host,
                             EmuHost::SaveMediaIO& saves) override;
    void unload(EmuHost::SaveMediaIO& saves) override;
    void bindPorts(const std::vector<EmuHost::PortBinding>& bindings,
                   EmuHost::HostPort& host) override;

    bool tick(bool hidden) override;
    bool serialize(std::vector<uint8_t>& out) override;
    bool unserialize(const uint8_t* data, size_t size) override;

    void syncSave() override;
    bool collectSaveMedia(EmuHost::SaveMediaIO& saves) override;

    int  readMemory(uint32_t offset, uint8_t* out, uint32_t length) override;
    void writeMemory(uint32_t offset, const uint8_t* data, uint32_t length) override;

    void setVideoSettings(float, float, float, bool, bool) override {}
    void setOverscan(bool) override {}
    bool applyRuntimeToggle(const std::string&, bool) override { return false; }
    int  readRuntimeToggle(const std::string&) override { return -1; }
    std::string readBootOption(const std::string&) override { return ""; }
    bool applyRateControl(double fillLevel) override;
    void syncCheats(const std::unordered_map<uint32_t, uint32_t>& table) override;

private:
    // libretro's C callbacks carry no context — sActive is the adopted
    // instance for the process lifetime of the loaded core.
    static bool onEnvironment(unsigned cmd, void* data);
    static void onVideoRefresh(const void* data, unsigned width, unsigned height, size_t pitch);
    static void onAudioSample(int16_t left, int16_t right);
    static size_t onAudioBatch(const int16_t* data, size_t frames);
    static void onInputPoll();
    static int16_t onInputState(unsigned port, unsigned device, unsigned index, unsigned id);
    static void onLog(int level, const char* fmt, ...);
    static LibretroBackend* sActive;

    struct CoreApi;

    // One dlopened core: handle, resolved entry points, self-reported
    // identity. `current_` is the core that boots and ticks; `pending_`
    // stages an adoption made while current_ still runs a game (the host
    // unloads before boot — the swap completes there).
    struct CoreRef {
        void* handle = nullptr;
        CoreApi* api = nullptr;
        std::string requestName;      // the exact adoption request (may be a path)
        std::string coreName;         // display identity ("snes9x"), path-independent
        std::string libraryName;      // core-reported
        std::string libraryVersion;   // core-reported
        bool needFullpath = false;
        bool initialized = false;     // retro_init has run
    };

    bool probeCore(const std::string& name, CoreRef& out);
    CoreApi* loadCoreSymbols(void* handle);
    void closeCore(CoreRef& core);
    const CoreRef& bootTarget() const { return pending_.handle ? pending_ : current_; }
    void pushResampled(const int16_t* frames, size_t count);
    void applyGeometry(const retro_game_geometry& geometry);

    CoreRef current_;
    CoreRef pending_;
    std::string systemId_;        // catalog system the adoption serves
    std::string versionString_;   // backing storage for version()

    bool loaded_ = false;         // a game is loaded on current_

    // Video
    int pixelFormat_ = 0;         // RETRO_PIXEL_FORMAT_*, set via environment
    std::vector<uint32_t> converted_;
    double frameWidth_ = 0, frameHeight_ = 0, aspectRatio_ = 0;

    // Audio: cores produce at their own rate; a linear resampler feeds the
    // host's 48 kHz ring. drcRate_ is the DRC-skewed destination rate.
    double sourceRate_ = 0;
    double drcRate_ = 48000.0;
    double resamplePhase_ = 0;
    int16_t prevL_ = 0, prevR_ = 0;
    bool havePrev_ = false;

    // Core options captured from SET_VARIABLES / SET_CORE_OPTIONS — value
    // defaults are answered back through GET_VARIABLE and the definitions
    // surface through capabilities().
    std::map<std::string, std::string> optionValues_;
    std::vector<EmuHost::OptionInfo> optionInfos_;
    bool optionsUpdated_ = false;

    // Cheats as per-frame writes into the memory window (action-replay
    // semantics, uniform with the mGBA backend).
    std::vector<std::pair<uint32_t, uint8_t>> cheatWrites_;

    bool hidden_ = false;
    EmuHost::HostPort* host_ = nullptr;
    // handles[logicalPort-1][retroPadId]; RetroPad ids equal the catalog's
    // positional bits by construction.
    static constexpr int kMaxPorts = 5;
    int keyHandles_[kMaxPorts][16];
};
