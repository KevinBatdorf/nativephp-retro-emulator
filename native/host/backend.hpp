// The engine seam: EmulatorHost owns every engine-neutral policy; a Backend
// owns one engine's mechanics and wires its callbacks to the given HostPort.
//
// No ares includes here, ever — this header (and everything in native/host/)
// must compile for a build that bundles no ares at all.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace EmuHost {

// Screen-node presentation geometry captured with each frame. Field meaning
// follows ares desktop-ui/program/platform.cpp:95-115; engines without
// distinct scale/aspect report {w, h, 1, 1, 1, 1, 0}.
struct FrameGeometry {
    double   width  = 0.0;
    double   height = 0.0;
    double   scaleX = 1.0;
    double   scaleY = 1.0;
    double   aspectX = 1.0;
    double   aspectY = 1.0;
    uint32_t rotation = 0;
};

// One physical controller port's resolved binding, produced by the host's
// device policy (registrations + the port-1 default pad + multitap fan-out).
// Backends translate this to engine terms and acquire input handles for each
// logical assignment; they never re-derive policy.
struct LogicalAssignment {
    int         logicalPort;   // 1-based logical (multitap on port 2 → 2,3,4,5)
    std::string device;        // device whose descriptor drives input caching
                               // (a multitap sub-port reads "Gamepad")
};
struct PortBinding {
    int         physicalPort;  // 1-based; a ports==0 system gets one pseudo-
                               // binding {0, "", {{1, ""}}} = built-in controls
    std::string device;        // effective device on the physical port; "" = disconnect
    bool        multitap = false;
    std::vector<LogicalAssignment> logical;
};

// Result of pre-boot ROM analysis. Analysis must be side-effect free w.r.t.
// any RUNNING game: a ROM rejected here leaves play untouched (the host only
// tears down after ok). `token` carries backend-private work (ares: the built
// cartridge pak) into the boot that follows.
struct Analysis {
    bool        ok = false;
    std::string error;       // human-readable reject reason (logged, not parsed)
    std::string title;
    std::string regionCsv;   // ROM's analyzed regions, "NTSC-J, NTSC-U" style CSV
    std::shared_ptr<void> token;
};

// Everything a boot needs, staged host-side. Pointers are borrowed for the
// duration of the call only.
struct BootSpec {
    std::string systemId;
    std::shared_ptr<void> token;                    // from analyze()
    std::string region;                             // host-resolved; "" = region-free
    const std::vector<uint8_t>* bios = nullptr;     // firmware override; empty = none
    const std::vector<uint8_t>* slot[2] = {nullptr, nullptr};  // staged slot ROMs
    const std::map<std::string, std::string>* bootOptions = nullptr;
    const std::vector<PortBinding>* bindings = nullptr;
    bool overscan = false;
};

struct BootResult {
    bool ok = false;
    bool slotConnected[2] = {false, false};
    // Which staged slot ROMs the boot consumed. Unconsumed slots (no slot
    // port on this cartridge, no slot support) stay staged for the next boot.
    bool slotConsumed[2] = {false, false};
};

// Battery-save persistence door. The host implements it over
// "<savePrefix>.<name>" files (save.ram, save.eeprom, …); backends call it
// wherever their engine seeds/flushes battery memory. read() returns empty when no file exists.
struct SaveMediaIO {
    virtual ~SaveMediaIO() = default;
    virtual std::vector<uint8_t> read(const std::string& name) = 0;
    virtual bool write(const std::string& name, const uint8_t* data, size_t size) = 0;
    // Filesystem path a write(name, …) lands at, for engines that must read
    // media by path (libretro need_fullpath cores). Empty = no file backing.
    virtual std::string pathFor(const std::string& name) { return {}; }
};

// What a backend can do for a given system. The host consults this to gate
// host-level features (rewind and run-ahead need serialize; cheat calls need
// cheats; …) and to answer capability
// introspection. A false here becomes an explicit error naming the backend —
// never a silent no-op.
struct OptionInfo {
    std::string key;                       // wrapper camelCase key or engine option name
    enum class Stage { Boot, Runtime } stage = Stage::Runtime;
    enum class Kind  { Boolean, Number, Enum, String } kind = Kind::Boolean;
    std::vector<std::string> values;       // Enum choices, else empty
    std::string current;                   // live value where the engine tracks one
};
struct Capabilities {
    bool serialize     = false;   // save states + host rewind + host run-ahead
    bool cheats        = false;
    bool memoryAccess  = false;
    bool rumble        = false;
    bool rateControl   = false;   // applyRateControl steers engine resampling
    bool slottedMedia  = false;   // SuFami / BS-X slots
    bool videoSettings = false;   // luminance/saturation/gamma/colorBleed
    bool multitap      = false;
    bool mouse         = false;
    std::vector<OptionInfo> options;
};

// Host services a running backend consumes. Implemented by EmulatorHost;
// every callback an engine fires funnels through here.
//
// Input handles: acquired during bindPorts() (buttonHandle/axisHandle), valid
// until the next bindPorts() or unload — the host clears its table right
// before calling bindPorts, so backends re-acquire every time. Sampling
// carries the press-latch contract: a software tap shorter than one poll
// stays visible for exactly one sample, and its release lands right after
// that first sample (see EmulatorHost for the mechanism).
class HostPort {
public:
    virtual ~HostPort() = default;

    // Input --------------------------------------------------------------
    virtual int     buttonHandle(int logicalPort, const std::string& button) = 0; // -1 unknown
    virtual bool    sampleButton(int handle) = 0;
    virtual int     axisHandle(int logicalPort, const std::string& axis) = 0;     // -1 unknown
    virtual int32_t consumeAxisDelta(int handle) = 0;   // relative delta, applies once

    // Sinks ----------------------------------------------------------------
    // pushFrame: any thread (engines deliver frames from worker threads).
    // strideWords is the source row pitch in 32-bit words; the host performs
    // the one stride-flattening copy into its frame buffer, so backends hand
    // over the engine's buffer directly — no second copy.
    // pushAudioFrame: one stereo pair, engine-mixed, BEFORE volume/balance —
    // the host applies those while inserting into its ring.
    virtual void pushFrame(const uint32_t* argb, uint32_t width, uint32_t height,
                           uint32_t strideWords, const FrameGeometry& geometry) = 0;
    virtual void pushAudioFrame(double left, double right) = 0;
    virtual void setRefreshRateHint(double hz) = 0;
    virtual void setRumble(uint16_t strong, uint16_t weak) = 0;

    // Cheat hot path -------------------------------------------------------
    // Consulted on every emulated bus read; a virtual call there would tax
    // the hottest loop in the process. The table pointer is stable for the
    // host's lifetime — backends cache it once and inline the
    // empty()-check + lookup.
    virtual const std::unordered_map<uint32_t, uint32_t>* cheatTable() const = 0;
};

// One emulation engine. Exactly one backend instance runs a system at a
// time; the host serializes every call, so backends need no internal locking
// beyond what their engine demands.
class Backend {
public:
    virtual ~Backend() = default;

    virtual const char* name() const = 0;      // "ares", "sameboy", "mgba", "libretro"
    virtual const char* version() const = 0;

    // Catalog ids this backend can boot, in any order. For modular builds
    // this reflects what actually loaded (ares: which core modules dlopen'd).
    virtual std::vector<std::string> systems() const = 0;
    virtual Capabilities capabilities(const std::string& systemId) const = 0;

    // Lifecycle ------------------------------------------------------------
    virtual Analysis   analyze(const std::string& systemId,
                               const uint8_t* rom, size_t size) = 0;
    virtual BootResult boot(const BootSpec& spec, HostPort& host, SaveMediaIO& saves) = 0;
    // Flushes battery media before teardown. Safe when nothing runs.
    virtual void unload(SaveMediaIO& saves) = 0;

    // (Re)bind controller ports on the live engine. Called by the host after
    // it clears its input-handle table: re-acquire every handle here.
    virtual void bindPorts(const std::vector<PortBinding>& bindings, HostPort& host) = 0;

    // Emulation ------------------------------------------------------------
    // One video frame. hidden = run-ahead hidden frame: suppress (or discard)
    // audio/video output but advance state.
    virtual bool tick(bool hidden) = 0;

    // True while the console's boot animation runs — the host fast-forwards
    // those frames hidden unless the bootAnimation option asks to play them.
    virtual bool inBootIntro() { return false; }

    virtual bool serialize(std::vector<uint8_t>& out) = 0;
    virtual bool unserialize(const uint8_t* data, size_t size) = 0;

    // Battery saves --------------------------------------------------------
    // syncSave: make battery memory current engine-side (ares root->save()).
    virtual void syncSave() = 0;
    virtual bool collectSaveMedia(SaveMediaIO& saves) = 0;

    // Memory window (offsets relative to the catalog's memBase) -------------
    virtual int  readMemory(uint32_t offset, uint8_t* out, uint32_t length) = 0;
    virtual void writeMemory(uint32_t offset, const uint8_t* data, uint32_t length) = 0;

    // Options / AV ----------------------------------------------------------
    virtual void setVideoSettings(float luminance, float saturation, float gamma,
                                  bool colorBleed, bool overscan) = 0;
    virtual void setOverscan(bool overscan) = 0;
    // Runtime toggle by wrapper key (colorEmulation, …). false = the running
    // core doesn't expose it (the host decides how loud to be about that).
    virtual bool applyRuntimeToggle(const std::string& key, bool value) = 0;
    virtual int  readRuntimeToggle(const std::string& key) = 0;   // 1/0, -1 absent
    // Boot-option readback from the LIVE engine ("Pixel Accuracy" → "true").
    virtual std::string readBootOption(const std::string& name) = 0;

    // Steer engine-side resampling toward a half-full host ring (dynamic rate
    // control). fillLevel ∈ [0,1]. Return false where the engine can't.
    virtual bool applyRateControl(double fillLevel) = 0;

    // Cheats are host-owned (parse + table); engines consume them one of two
    // ways. Pull: intercept bus reads through HostPort::cheatTable (ares).
    // Push: mirror the table into engine cheat slots here — the host calls
    // this after every add/remove/clear and after boot. Default no-op serves
    // the pull style.
    virtual void syncCheats(const std::unordered_map<uint32_t, uint32_t>& table) { (void)table; }

    // Bring-your-own engines: a requested backend name no registered engine
    // answers to is offered to each backend as a dynamic core to adopt for
    // `systemId` ("snes9x" → the loader probes libsnes9x_libretro_android.so).
    // Return true only when the core loaded and can serve the system; the
    // backend then boots that core until the next adoption or unload.
    virtual bool adoptDynamicCore(const std::string& name, const std::string& systemId) {
        (void)name; (void)systemId;
        return false;
    }

    // Engine-declared options — the escape hatch for keys only the engine
    // knows (libretro core options). A set is legal only when the engine
    // itself declares the key AND the value; behavior belongs to the core
    // author. Returns "" on success, else the human-readable refusal the
    // bridge surfaces as UNSUPPORTED_OPTION. `staged` targets the engine the
    // next boot uses; false targets the running one.
    virtual std::string setEngineOption(const std::string& key, const std::string& value,
                                        bool staged) {
        (void)key; (void)value; (void)staged;
        return std::string(name()) + " declares no engine options — its settings are the "
               "typed config keys";
    }

    // The engine-declared schema behind setEngineOption, each entry carrying
    // its live value in `current`. Empty for engines whose settings are the
    // typed config.
    virtual std::vector<OptionInfo> engineOptions() const { return {}; }
};

} // namespace EmuHost
