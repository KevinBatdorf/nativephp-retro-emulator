// EmulatorHost — the engine-neutral core both platform bridges marshal to.
// Engines plug in underneath via backend.hpp.
//
// Threading: functions documented "emulation thread only" must run on the
// platform's emu/render thread; the rest are safe from bridge threads.
#pragma once

#include "backend.hpp"
#include "system_catalog.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace EmuHost {

class EmulatorHost final : public HostPort {
public:
    static constexpr int kMaxPorts = 5;   // multitap territory

    EmulatorHost();
    ~EmulatorHost() override;

    // Factory state in place, keeping this pointer valid for stored host
    // references — the Stop semantics both platforms share.
    // Emulation thread only.
    void reset();

    // System staging + ROM-first boot ------------------------------------
    // Stage a system declaration; nothing boots until loadRom. `preferred`
    // picks the engine ("" runs the built-in ares). Returns false for ids
    // no registered backend claims (UNSUPPORTED_SYSTEM at the bridge).
    // biosPath: optional firmware override, read here (empty/unreadable
    // logs and stages nothing).
    bool stageSystem(const std::string& systemId, const std::string& biosPath,
                     const std::string& preferred = "");

    // Stage a slotted-media ROM (0 = SuFami A / BS slot, 1 = SuFami B) for
    // the next loadRom. Empty bytes clear the slot. Emulation thread only.
    void stageSlot(int index, const uint8_t* rom, size_t size);
    bool isSlotConnected(int index) const;   // test seam

    // Stage a boot option by engine option name ("Pixel Accuracy"), applied
    // before the next boot. Survives ROM swaps like device registrations.
    void stageBootOption(const std::string& name, const std::string& value);
    // Live boot-option value from the running engine ("" when nothing runs).
    std::string readBootOption(const std::string& name);

    // Boot the staged system with this ROM — the ONE boot path, first load
    // and every swap alike. Returns 1 success; 0 rejected before teardown (a
    // running game is untouched); -1 a later failure left the emulator
    // cleanly stopped. Emulation thread only.
    int loadRom(const uint8_t* rom, size_t size, const std::string& savePrefix,
                const std::string& regionOverride, const std::string& preferredRegions);

    // Write battery memory to disk under the loadRom prefix. Emulation
    // thread only. False when nothing was persisted.
    bool flushSaves();

    // Emulation ------------------------------------------------------------
    // One tick: consumes pending device/remap changes, then (unless paused)
    // rate control, rewind, and the frame (with run-ahead when enabled).
    // Returns false when no ROM is loaded or paused. Emulation thread only.
    bool tick();

    void pause();    // any thread
    void resume();   // any thread

    // Input ------------------------------------------------------------------
    void     setInput(int port, uint32_t bits);          // hardware mask, any thread
    uint32_t combinedInput(int port) const;              // hw|sw, test seam

    // Status-string returns: "" success, else the category-A code the
    // bridges raise ("SYSTEM_NOT_LOADED", "INVALID_PARAMETERS",
    // "UNSUPPORTED_DEVICE", "UNKNOWN_BUTTON:<name>").
    std::string connectDevice(const std::string& systemId, int port,
                              const std::string& device);
    std::vector<int> devicePorts(const std::string& systemId, int physical);
    std::string pressButton(int port, const std::string& name, bool down);
    std::string setAxis(int port, const std::string& name, int value);
    std::string aimAt(int port, float nx, float ny);
    std::string setInputMapping(int port,
                                const std::vector<std::string>& emulated,
                                const std::vector<std::string>& source);
    int getButtonBit(int port, const std::string& name);     // test seam
    int getAxisAccum(int port, const std::string& name);     // test seam
    std::string pressedButtons(int port);                     // comma-joined
    std::string portsJson();                                  // from staging on

    // AV options ---------------------------------------------------------
    void setAudio(float volume, float balance);               // any thread
    void setRawAudio(bool raw);                               // any thread
    void setVideo(float luminance, float saturation, float gamma,
                  bool colorBleed, bool overscan);            // emulation thread
    void setCoreBoolean(const std::string& key, bool value);  // emulation thread
    int  coreBoolean(const std::string& key);                 // test seam

    // Video out ------------------------------------------------------------
    // Latest-frame access under the frame lock. withDirtyFrame runs `fn`
    // only when a fresh frame is pending and clears the flag (the Vulkan
    // staging path); copyLatestFrame copies whatever is newest regardless
    // (the iOS pull path). Geometry is all zeros before the first frame.
    bool withDirtyFrame(const std::function<void(const uint32_t*, uint32_t, uint32_t)>& fn);
    bool copyLatestFrame(uint32_t* out, size_t capacity,
                         uint32_t* width, uint32_t* height);
    uint32_t frameWidth() const;
    uint32_t frameHeight() const;
    void videoGeometry(double out[7]);
    double refreshRateHint() const;                           // any thread

    // Audio out ------------------------------------------------------------
    size_t readAudio(float* out, size_t capacity);            // drain thread

    // A sound stop within the first phase-2 frames means boot-ROM audio leaked.
    int  bootSkipExtraFrames() const { return bootSkipLastExtra_; }
    bool bootSkipStoppedOnSound() const { return bootSkipLastSound_; }

    // Save states ----------------------------------------------------------
    // Host file I/O + a backend tag header; legacy untagged files load as
    // ares (see stateTag notes in the .cpp). Emulation thread only.
    bool stateSave(const std::string& path);
    bool stateLoad(const std::string& path);

    // Memory window ----------------------------------------------------------
    int  readMemory(uint32_t address, uint8_t* out, uint32_t length);
    void writeMemory(uint32_t address, const uint8_t* data, uint32_t length);

    // Cheats — emulation thread only (the table is read inside tick) ---------
    bool addCheat(const std::string& code);
    bool removeCheat(const std::string& code);
    void clearCheats();

    // Rewind / run-ahead / pacing -------------------------------------------
    void configureRewind(bool enabled, int bufferSeconds);    // emulation thread
    int  toggleRewind();                                      // 1/0/-1
    void setRunAhead(bool enabled);                           // emulation thread
    void setFastForward(bool active);                         // any thread
    void setDynamicRateControl(bool enabled);                 // any thread

    // Rumble ----------------------------------------------------------------
    void     setRumbleEnabled(bool enabled);                  // any thread
    uint32_t rumbleState() const;                             // any thread

    // Capabilities ------------------------------------------------------------
    // Answered against the engine a call would actually hit (active first,
    // else staged) so bridges can reject an unsupported option loudly and
    // synchronously — never a silent no-op on an engine that lacks the door.
    bool videoSettingsSupported() const;
    // A toggle key outside the engine's declared option set is unsupported;
    // a declared key may still be refused by the engine at apply time.
    bool toggleSupported(const std::string& key) const;
    // The engine serving calls right now: active, else staged, else "".
    std::string backendName() const;
    // {"engines":[…], "gb":{"backends":["ares","sameboy"]}, …} for every
    // available system — the introspection the PHP layer surfaces. No
    // "default" key on purpose: unnamed boots run the built-in engine.
    static std::string backendsJson();
    // Engine-declared options (libretro cores). Set returns "" or the
    // refusal message the bridge surfaces as UNSUPPORTED_OPTION; `staged`
    // targets the engine the next boot uses. The JSON lists the schema:
    // [{"key","choices",[…],"default","current"}, …].
    std::string setEngineOption(const std::string& key, const std::string& value, bool staged);
    std::string engineOptionsJson() const;

    // Metadata ---------------------------------------------------------------
    std::string region() const;
    bool systemStaged() const { return stagedSystem_ != nullptr; }
    bool romLoaded() const { return romLoaded_; }
    // Extensions valid for an id — empty when no registered backend claims it.
    // Static: answered from catalog + registry, needed before any host exists
    // (system lists render on plain screens long before a surface).
    static std::vector<std::string> systemExtensionsFor(const std::string& systemId);

    // HostPort (backend-facing) ---------------------------------------------
    int     buttonHandle(int logicalPort, const std::string& button) override;
    bool    sampleButton(int handle) override;
    int     axisHandle(int logicalPort, const std::string& axis) override;
    int32_t consumeAxisDelta(int handle) override;
    void pushFrame(const uint32_t* argb, uint32_t width, uint32_t height,
                   uint32_t strideWords, const FrameGeometry& geometry) override;
    void pushAudioFrame(double left, double right) override;
    void setRefreshRateHint(double hz) override;
    void setRumble(uint16_t strong, uint16_t weak) override;
    const std::unordered_map<uint32_t, uint32_t>* cheatTable() const override;

private:
    // Device policy → physical bindings with logical fan-out.
    std::vector<PortBinding> resolveBindings();
    // Descriptor of the device at a LOGICAL port (multitap sub-port reads
    // "Gamepad"); false when nothing is connected there.
    bool connectedDescriptor(int port, SystemCatalog::DeviceDescriptor& out);
    // Clear the handle table + re-bind the backend's ports. Emulation thread.
    void rebindPorts();
    // Re-resolve every handle's read bit from defaults + the stored remap.
    void applyInputRemap();
    // Tear down the running game, keeping registrations + AV prefs.
    // flushSaves is true only on the ROM-swap path: destroy/reset must not
    // write battery media — the platform layers flush explicitly first when
    // they want persistence.
    void unloadGame(bool flushSaves);
    void rewindRun();

    // Staged engine + system (bridge thread writes at stage, emu thread
    // reads at boot).
    // stagedBackend_ is what the NEXT boot uses; activeBackend_ is what runs
    // now — re-staging over a running game must not redirect teardown.
    Backend* stagedBackend_ = nullptr;
    Backend* activeBackend_ = nullptr;
    const SystemCatalog::System* stagedSystem_ = nullptr;
    const SystemCatalog::System* activeSystem_ = nullptr;
    std::vector<uint8_t> biosBytes_;
    std::vector<uint8_t> runAheadScratch_;

    bool systemLoaded_ = false;
    bool romLoaded_    = false;
    std::string romRegion_;
    std::string savePrefix_;
    // Overscan borders trimmed by default; setVideo(overscan: true) shows
    // them. Reapplied to the engine on every boot.
    bool overscan_ = false;

    std::vector<uint8_t> stagedSlot_[2];
    bool slotConnected_[2] = {false, false};
    std::map<std::string, std::string> stagedBootOptions_;

    // Video hand-off. Engines deliver frames from worker threads; the emu/GL
    // thread uploads. Guarded by frameMutex_.
    mutable std::mutex frameMutex_;
    std::vector<uint32_t> frameBuffer_;
    uint32_t frameWidth_  = 0;
    uint32_t frameHeight_ = 0;
    FrameGeometry geometry_;
    bool frameDirty_ = false;

    // Audio ring — engine-mixed stereo floats, volume/balance applied at
    // insertion. ~125 ms cap; overflow drops the OLDEST samples so backlog
    // self-heals instead of persisting.
    static constexpr size_t kAudioCap = 12000u;
    std::atomic<uint64_t> audioDropped_ {0};     // floats discarded at cap
    std::atomic<uint64_t> audioUnderruns_ {0};   // reads that found nothing
    int audioStatTicks_ = 0;
    mutable std::mutex audioMutex_;
    std::vector<float> audioRing_;
    std::atomic<float> volume_  {1.0f};
    std::atomic<float> balance_ {0.0f};

    // GB mixer parks off silence; stepping that level reaches speakers as infrasound.
    std::atomic<bool> rawAudio_ {false};

    // Boot-skip phase 2: AV runs but is discarded; audio peak steers the stop.
    // Peak is measured DC-free — rawAudio boots sit on the GB pedestal, and a
    // rail is not sound. The tick that trips the guard is buffered and
    // replayed so the triggering attack is not eaten. Emulation thread only.
    std::atomic<bool> bootSkipDiscard_ {false};
    std::atomic<bool> bootSkipVideoHold_ {false};
    int  bootSkipLastExtra_ = 0;
    bool bootSkipLastSound_ = false;
    double bootSkipAudioPeak_ = 0.0;
    double bootSkipDcL_ = 0.0, bootSkipDcR_ = 0.0;
    bool bootSkipDcPrimed_ = false;
    std::vector<float> bootSkipTickAudio_;
    uint64_t frameChecksum();
    void captureFrame(std::vector<uint32_t>& out);
    bool audioHighpass_ = false;
    bool hpPrimed_ = false;
    double hpX1_[2] {}, hpX2_[2] {}, hpY1_[2] {}, hpY2_[2] {};   // emulation thread only

    // Input. Two per-port masks OR'd at poll time: hwMask (hardware pads,
    // any thread) and swMask (software presses). Press latch: unsampledPress
    // marks sw bits not yet seen by a poll; deferredRelease applies right
    // after the bit's first sample so every tap is visible for one poll.
    std::atomic<uint32_t> hwMask_[kMaxPorts] {};
    std::atomic<uint32_t> swMask_[kMaxPorts] {};
    std::atomic<uint32_t> unsampledPress_[kMaxPorts] {};
    std::atomic<uint32_t> deferredRelease_[kMaxPorts] {};

    // Input handles the backend acquires at bind time. `bit` is the slot the
    // button reads post-remap; `defaultBit` its unremapped value. Mutated on
    // the emulation thread only (rebind + remap), sampled there too.
    struct ButtonEntry {
        int         port;
        std::string name;      // lowercased
        uint32_t    bit;
        uint32_t    defaultBit;
    };
    std::vector<ButtonEntry> buttonTable_;
    struct AxisEntry {
        int         port;
        std::string name;
    };
    std::vector<AxisEntry> axisTable_;

    // Axis accumulate-and-consume (mouse deltas) + the aimAt shadow cursor
    // (mirrors the engine's internal cursor so aimAt can feed the delta to
    // reach an absolute position).
    static constexpr int32_t kAimW = 256, kAimH = 240;
    mutable std::mutex axisMutex_;
    std::unordered_map<std::string, int32_t> axisAccum_[kMaxPorts];
    int32_t aimX_[kMaxPorts] {};
    int32_t aimY_[kMaxPorts] {};

    // Device registration by PHYSICAL port; "" = nothing. Persists across
    // boots. deviceDirty defers the engine rebind to the emulation thread.
    std::string connectedDevice_[kMaxPorts];
    mutable std::mutex deviceMutex_;
    std::atomic<bool> deviceDirty_ {false};

    // Per-port remap: lowercased button name → the bit it should read.
    std::unordered_map<int, std::unordered_map<std::string, uint32_t>> inputRemap_;
    std::mutex inputRemapMutex_;
    std::atomic<bool> inputRemapDirty_ {false};

    std::atomic<bool> paused_ {false};

    // Cheats. `cheats_` keeps per-code pairs so removeCheat works;
    // `cheatLookup_` is the merged table the engine's hot path reads.
    std::map<std::string, std::map<uint32_t, uint32_t>> cheats_;
    std::unordered_map<uint32_t, uint32_t> cheatLookup_;
    void rebuildCheatLookup();

    // Rewind — desktop-ares semantics over backend serialize blobs.
    struct Rewind {
        bool enabled   = false;
        bool rewinding = false;
        uint32_t counter   = 0;
        uint32_t frequency = 10;    // capture every N frames
        uint32_t length    = 100;   // history cap (~16.7 s)
        std::vector<std::vector<uint8_t>> history;
    } rewind_;

    bool runAheadEnabled_ = false;
    std::atomic<bool> fastForwardActive_ {false};

    std::atomic<bool> rumbleEnabled_ {false};
    std::atomic<uint32_t> rumbleState_ {0};

    std::atomic<bool> dynamicRateControl_ {true};
    std::atomic<double> refreshRateHint_ {0.0};
};

} // namespace EmuHost
