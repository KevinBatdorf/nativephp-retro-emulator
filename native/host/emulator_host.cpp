// The engine-neutral half of the emulator: every policy here is shared by
// both platform bridges and every backend. Behavioral reference points cite
// desktop-ares where the policy is a port of its semantics.
#include "emulator_host.hpp"

#include "backend_registry.hpp"
#include "host_log.hpp"

#include "cheat_parse.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>

namespace EmuHost {

namespace {

auto toLower(std::string s) -> std::string {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

auto readFile(const std::string& path) -> std::vector<uint8_t> {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return {};
    std::fseek(f, 0, SEEK_END);
    long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (size <= 0) { std::fclose(f); return {}; }
    std::vector<uint8_t> data((size_t)size);
    std::fread(data.data(), 1, (size_t)size, f);
    std::fclose(f);
    return data;
}

auto writeFile(const std::string& path, const uint8_t* data, size_t size) -> bool {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    size_t written = std::fwrite(data, 1, size, f);
    std::fclose(f);
    return written == size;
}

// Battery-save persistence over "<savePrefix>.<name>" files — host-owned;
// backends walk their engine's media and
// call through this. An empty prefix disables persistence.
struct FileSaveMediaIO final : SaveMediaIO {
    std::string prefix;
    explicit FileSaveMediaIO(std::string p) : prefix(std::move(p)) {}
    std::vector<uint8_t> read(const std::string& name) override {
        if (prefix.empty()) return {};
        return readFile(prefix + "." + name);
    }
    bool write(const std::string& name, const uint8_t* data, size_t size) override {
        if (prefix.empty()) return false;
        return writeFile(prefix + "." + name, data, size);
    }
    std::string pathFor(const std::string& name) override {
        if (prefix.empty()) return {};
        return prefix + "." + name;
    }
};

// nall::split_and_strip equivalent for the "NTSC-J, NTSC-U" CSV form used by
// ROM region lists and preferred-region settings.
auto splitAndStrip(const std::string& csv) -> std::vector<std::string> {
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= csv.size()) {
        size_t comma = csv.find(',', start);
        if (comma == std::string::npos) comma = csv.size();
        size_t a = start, b = comma;
        while (a < b && (csv[a] == ' ' || csv[a] == '\t')) a++;
        while (b > a && (csv[b - 1] == ' ' || csv[b - 1] == '\t')) b--;
        if (b > a) out.push_back(csv.substr(a, b - a));
        if (comma == csv.size()) break;
        start = comma + 1;
    }
    return out;
}

auto contains(const std::vector<std::string>& list, const std::string& value) -> bool {
    for (auto& entry : list) if (entry == value) return true;
    return false;
}

// Pick the boot region — a port of desktop-ares Emulator::region()
// (emulator.cpp:40-60); NTSC-U/NTSC-J preferences also match a plain "NTSC"
// entry. Extensions beyond the reference: a non-empty override wins outright
// (junk homebrew headers), and an empty ROM list falls back to the first
// preference the system supports, then the system's first region.
auto resolveRegion(const SystemCatalog::System& sys,
                   const std::string& romRegionCsv,
                   const std::string& regionOverride,
                   const std::string& preferredCsv) -> std::string {
    if (sys.regions.empty()) return {};
    if (!regionOverride.empty()) return regionOverride;

    auto preferredRegions = splitAndStrip(preferredCsv.empty() ? "NTSC-U" : preferredCsv);
    auto regions = splitAndStrip(romRegionCsv);

    if (!regions.empty()) {
        for (auto& prefer : preferredRegions) {
            if (contains(regions, prefer)) return prefer;
            if (prefer == "NTSC-U" || prefer == "NTSC-J") {
                if (contains(regions, "NTSC")) return "NTSC";
            }
        }
        return regions.front();
    }

    for (auto& prefer : preferredRegions) {
        if (contains(sys.regions, prefer)) return prefer;
    }
    return sys.regions.front();
}

// Bit for a button name in a descriptor, case-insensitively; false if absent.
auto bitForButtonName(const SystemCatalog::DeviceDescriptor& desc,
                      const std::string& name, uint32_t& out) -> bool {
    auto lname = toLower(name);
    for (auto& [key, bit] : desc.buttons)
        if (toLower(key) == lname) { out = bit; return true; }
    return false;
}

auto jsonStringArray(const std::vector<std::string>& items) -> std::string {
    std::string s = "[";
    for (auto& it : items) { if (s.size() > 1) s += ","; s += "\"" + it + "\""; }
    return s + "]";
}

// Button names of a descriptor, ordered by bit (stable output).
auto orderedButtons(const SystemCatalog::DeviceDescriptor& desc)
    -> std::vector<std::string> {
    std::vector<std::pair<uint32_t, std::string>> ordered;
    for (auto& [name, bit] : desc.buttons) ordered.push_back({bit, name});
    std::sort(ordered.begin(), ordered.end());
    std::vector<std::string> names;
    for (auto& [bit, name] : ordered) names.push_back(name);
    return names;
}

// Save-state container header. Files begin "NPEB1" + u8 backend-name length
// + name + the engine payload; untagged legacy files load as ares. A tagged file from a different engine fails
// loudly — engine states are not portable between engines.
constexpr char kStateMagic[5] = {'N', 'P', 'E', 'B', '1'};

} // namespace

EmulatorHost::EmulatorHost() {
    for (int i = 0; i < kMaxPorts; i++) {
        aimX_[i] = kAimW / 2;
        aimY_[i] = kAimH / 2;
    }
}

EmulatorHost::~EmulatorHost() {
    // No battery flush on destroy — the platform layers call FlushSaves
    // explicitly first when they want persistence.
    if (systemLoaded_) unloadGame(false);
}

void EmulatorHost::reset() {
    if (systemLoaded_) unloadGame(false);
    // Runs unconditionally: software presses, cheats and pause are all
    // settable from staging on, so they must clear even when no game ever
    // booted. This pointer stays valid for the host layers' stored references.
    stagedBackend_ = nullptr;
    activeBackend_ = nullptr;
    stagedSystem_  = nullptr;
    activeSystem_  = nullptr;
    biosBytes_.clear();
    romRegion_.clear();
    savePrefix_.clear();
    overscan_ = false;
    stagedSlot_[0].clear();
    stagedSlot_[1].clear();
    stagedBootOptions_.clear();
    {
        std::lock_guard<std::mutex> lock(frameMutex_);
        frameBuffer_.clear();
        frameWidth_ = frameHeight_ = 0;
        geometry_ = FrameGeometry{};
        frameDirty_ = false;
    }
    volume_.store(1.0f, std::memory_order_relaxed);
    balance_.store(0.0f, std::memory_order_relaxed);
    for (int i = 0; i < kMaxPorts; i++) {
        hwMask_[i].store(0, std::memory_order_relaxed);
        swMask_[i].store(0, std::memory_order_relaxed);
        unsampledPress_[i].store(0, std::memory_order_relaxed);
        deferredRelease_[i].store(0, std::memory_order_relaxed);
        connectedDevice_[i].clear();
        aimX_[i] = kAimW / 2;
        aimY_[i] = kAimH / 2;
    }
    cheats_.clear();
    rebuildCheatLookup();
    paused_.store(false, std::memory_order_relaxed);
    refreshRateHint_.store(0.0, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(audioMutex_);
        audioRing_.clear();
    }
    buttonTable_.clear();
    axisTable_.clear();
    {
        std::lock_guard<std::mutex> lock(axisMutex_);
        for (auto& m : axisAccum_) m.clear();
    }
    {
        std::lock_guard<std::mutex> lock(inputRemapMutex_);
        inputRemap_.clear();
    }
    inputRemapDirty_.store(false, std::memory_order_relaxed);
    deviceDirty_.store(false, std::memory_order_relaxed);
    rewind_ = Rewind{};
    runAheadEnabled_ = false;
    fastForwardActive_.store(false, std::memory_order_relaxed);
    rumbleEnabled_.store(false, std::memory_order_relaxed);
    rumbleState_.store(0, std::memory_order_relaxed);
    dynamicRateControl_.store(true, std::memory_order_relaxed);
}

// --- staging + boot ---------------------------------------------------------

bool EmulatorHost::stageSystem(const std::string& systemId,
                               const std::string& biosPath,
                               const std::string& preferred) {
    auto* sys = SystemCatalog::find(systemId);
    auto* backend = Backends::forSystem(systemId, preferred);
    if (!sys || !backend) {
        EMUHOST_LOGE("unsupported system: %s", systemId.c_str());
        return false;
    }

    // An optional dev-supplied BIOS travels with the staging (gba may
    // override its embedded open BIOS with a real dump).
    biosBytes_.clear();
    if (!biosPath.empty()) {
        auto file = readFile(biosPath);
        if (!file.empty()) {
            biosBytes_ = std::move(file);
            EMUHOST_LOGI("bios staged: %s (%zu bytes)", biosPath.c_str(), biosBytes_.size());
        } else {
            EMUHOST_LOGE("bios not readable: %s", biosPath.c_str());
        }
    }

    // Stage only — re-staging over a running core is legal; the running game
    // continues until the next loadRom boots the new declaration.
    stagedBackend_ = backend;
    stagedSystem_  = sys;
    EMUHOST_LOGI("%s system staged (backend %s)", sys->id.c_str(), backend->name());
    return true;
}

void EmulatorHost::stageSlot(int index, const uint8_t* rom, size_t size) {
    if (index < 0 || index > 1) return;
    if (!rom || size == 0) {
        stagedSlot_[index].clear();
        return;
    }
    stagedSlot_[index].assign(rom, rom + size);
}

bool EmulatorHost::isSlotConnected(int index) const {
    if (index < 0 || index > 1) return false;
    return slotConnected_[index];
}

void EmulatorHost::stageBootOption(const std::string& name, const std::string& value) {
    stagedBootOptions_[name] = value;
}

std::string EmulatorHost::readBootOption(const std::string& name) {
    if (!systemLoaded_ || !activeBackend_) return "";
    return activeBackend_->readBootOption(name);
}

void EmulatorHost::unloadGame(bool flushSaves) {
    if (activeBackend_) {
        FileSaveMediaIO saves{flushSaves ? savePrefix_ : std::string{}};
        activeBackend_->unload(saves);
    }

    systemLoaded_ = false;
    romLoaded_    = false;
    slotConnected_[0] = slotConnected_[1] = false;
    buttonTable_.clear();
    axisTable_.clear();
    // connectedDevice_[] intentionally survives (registrations persist across
    // a reboot); the swMask does not — a device teardown drops held buttons.
    for (int i = 0; i < kMaxPorts; i++) {
        swMask_[i].store(0, std::memory_order_relaxed);
        unsampledPress_[i].store(0, std::memory_order_relaxed);
        deferredRelease_[i].store(0, std::memory_order_relaxed);
    }
    {
        std::lock_guard<std::mutex> lock(audioMutex_);
        audioRing_.clear();
    }
    // Cheats, the rewind timeline, the stale refresh hint, and any paused
    // flag belong to the outgoing game.
    cheats_.clear();
    rebuildCheatLookup();
    rewind_.rewinding = false;
    rewind_.counter   = 0;
    rewind_.history.clear();
    refreshRateHint_.store(0.0, std::memory_order_relaxed);
    paused_.store(false, std::memory_order_relaxed);
}

int EmulatorHost::loadRom(const uint8_t* rom, size_t size,
                          const std::string& savePrefix,
                          const std::string& regionOverride,
                          const std::string& preferredRegions) {
    if (!stagedBackend_ || !stagedSystem_) {
        EMUHOST_LOGE("loadRom: no system staged");
        return 0;
    }
    if (!rom || size == 0) {
        EMUHOST_LOGE("loadRom: empty ROM");
        return 0;
    }

    // Analyze BEFORE any teardown — a ROM that fails analysis must leave a
    // running game untouched (failures after teardown starts end in the
    // clean stopped state instead).
    auto analysis = stagedBackend_->analyze(stagedSystem_->id, rom, size);
    if (!analysis.ok) {
        EMUHOST_LOGE("loadRom: %s", analysis.error.c_str());
        return 0;
    }

    auto region = resolveRegion(*stagedSystem_, analysis.regionCsv,
                                regionOverride, preferredRegions);
    EMUHOST_LOGI("ROM: title='%s' regions='%s' → boot %s %s",
                 analysis.title.c_str(), analysis.regionCsv.c_str(),
                 stagedSystem_->id.c_str(),
                 region.empty() ? "(region-free)" : region.c_str());

    // Fresh core per game, like desktop — the one teardown that flushes
    // battery media.
    if (systemLoaded_) unloadGame(true);

    activeBackend_ = stagedBackend_;
    activeSystem_  = stagedSystem_;
    audioHighpass_ = stagedSystem_->id == "gb" || stagedSystem_->id == "gbc";
    hpPrimed_ = false;
    {
        auto it = stagedBootOptions_.find("rawAudio");
        const bool raw = it != stagedBootOptions_.end()
                         && (it->second == "true" || it->second == "1");
        rawAudio_.store(raw, std::memory_order_relaxed);
    }

    auto bindings = resolveBindings();

    BootSpec spec;
    spec.systemId    = stagedSystem_->id;
    spec.token       = analysis.token;
    spec.region      = region;
    spec.bios        = &biosBytes_;
    spec.slot[0]     = &stagedSlot_[0];
    spec.slot[1]     = &stagedSlot_[1];
    spec.bootOptions = &stagedBootOptions_;
    spec.bindings    = &bindings;
    spec.overscan    = overscan_;

    savePrefix_ = savePrefix;   // seed reads through it during boot
    FileSaveMediaIO saves{savePrefix_};

    // Stated explicitly on every boot: engines act on this option's value,
    // and an absent key would leave their per-core flags at stale values.
    if (!stagedBootOptions_.count("bootAnimation")) {
        stagedBootOptions_["bootAnimation"] = "false";
    }

    auto result = activeBackend_->boot(spec, *this, saves);
    if (!result.ok) {
        unloadGame(false);   // nothing booted; nothing to persist
        return -1;
    }

    for (int i = 0; i < 2; i++) {
        slotConnected_[i] = result.slotConnected[i];
        if (result.slotConsumed[i]) stagedSlot_[i].clear();
    }

    // The backend bound ports during boot; align the remap with the fresh
    // handle table and clear the flags.
    applyInputRemap();
    inputRemapDirty_.store(false, std::memory_order_relaxed);
    deviceDirty_.store(false, std::memory_order_relaxed);

    systemLoaded_ = true;
    romLoaded_    = true;

    bootSkipLastExtra_ = 0;
    bootSkipLastSound_ = false;
    {
        auto it = stagedBootOptions_.find("bootAnimation");
        const bool play = it != stagedBootOptions_.end()
                          && (it->second == "true" || it->second == "1");
        if (!play) {
            // The skip listens on cleaned audio regardless of rawAudio, so both variants stop at the same frame.
            const bool rawCfg = rawAudio_.load(std::memory_order_relaxed);
            if (rawCfg) activeBackend_->applyRuntimeToggle("rawAudio", false);
            // Phase 1 — to the boot ROM handoff. Consumes the ding and tail.
            int guard = 0;
            while (activeBackend_->inBootIntro() && ++guard <= 600) {
                activeBackend_->tick(true);
            }
            // Phase 2 — games display the handoff screen's logo until they redraw
            // it; consume that, stopping at the first new picture or sound.
            int extra = 0;
            if (guard) {
                bootSkipDiscard_.store(true, std::memory_order_relaxed);
                bootSkipAudioPeak_ = 0.0;
                bootSkipDcPrimed_ = false;
                bootSkipTickAudio_.clear();
                activeBackend_->tick(false);
                std::vector<uint32_t> baseline, cur, lastFrame;
                captureFrame(baseline);
                // Threaded ares screens deliver ~10 ms late; an empty baseline disarms every video exit.
                for (int wait = 0; wait < 25 && baseline.empty(); wait++) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                    captureFrame(baseline);
                }
                // Content over backdrop is the game's first screen; a logo
                // fade only touches the logo's own pixels.
                uint32_t background = 0;
                {
                    std::unordered_map<uint32_t, uint32_t> histo;
                    uint32_t best = 0;
                    for (uint32_t px : baseline) {
                        if (++histo[px] > best) { best = histo[px]; background = px; }
                    }
                }
                int stable = 0;
                int unchangedRun = 0;
                int loudTicks = 0;
                bool sound = false;
                std::vector<float> preroll;
                while (++extra <= 180) {
                    bootSkipAudioPeak_ = 0.0;
                    bootSkipTickAudio_.clear();
                    activeBackend_->tick(false);
                    // One loud tick is a rawAudio pedestal step lagging the DC
                    // tracker; real sound stays loud tick after tick.
                    loudTicks = bootSkipAudioPeak_ > 0.02 ? loudTicks + 1 : 0;
                    preroll.insert(preroll.end(), bootSkipTickAudio_.begin(),
                                   bootSkipTickAudio_.end());
                    const size_t cap = 4 * 4096;
                    if (preroll.size() > cap) {
                        preroll.erase(preroll.begin(),
                                      preroll.begin() + (preroll.size() - cap));
                    }
                    if (loudTicks >= 2) { sound = true; break; }
                    captureFrame(cur);
                    if (cur.size() == baseline.size() && !baseline.empty()) {
                        size_t drawn = 0;
                        bool changed = false;
                        for (size_t i = 0; i < cur.size(); i++) {
                            if (cur[i] != baseline[i]) {
                                changed = true;
                                if (baseline[i] == background) drawn++;
                            }
                        }
                        if (drawn > baseline.size() / 200) break;
                        if (changed) {
                            unchangedRun = 0;
                            stable = (cur == lastFrame) ? stable + 1 : 0;
                            if (stable >= 8) break;
                        } else if (++unchangedRun >= 75) {
                            // Past any logo hold: the game is loading dark.
                            break;
                        }
                    }
                    lastFrame = cur;
                }
                bootSkipDiscard_.store(false, std::memory_order_relaxed);
                if (sound) {
                    // Replay, or the skip eats the triggering sound's attack.
                    for (size_t i = 0; i + 1 < preroll.size(); i += 2) {
                        pushAudioFrame(preroll[i], preroll[i + 1]);
                    }
                }
                // A blank LCD presented at load flashes against the black surface.
                bootSkipVideoHold_.store(true, std::memory_order_relaxed);
                std::vector<uint32_t> held;
                for (int hold = 0; hold < 60; hold++) {
                    captureFrame(held);
                    if (held.empty()) break;
                    std::unordered_map<uint32_t, uint32_t> histo;
                    uint32_t modal = 0;
                    for (uint32_t px : held) {
                        if (++histo[px] > modal) modal = histo[px];
                    }
                    if (modal < held.size() - held.size() / 200) break;
                    activeBackend_->tick(false);
                }
                bootSkipVideoHold_.store(false, std::memory_order_relaxed);
                bootSkipTickAudio_.clear();
                bootSkipLastExtra_ = extra;
                bootSkipLastSound_ = sound;
            }
            if (rawCfg) activeBackend_->applyRuntimeToggle("rawAudio", true);
            if (guard) EMUHOST_LOGI("boot animation skipped (%d+%d frames%s)", guard,
                                    extra, bootSkipLastSound_ ? ", sound" : "");
        }
    }

    // Cheats staged before the first boot reach push-style engines here;
    // ares reads the table directly.
    if (!cheatLookup_.empty()) activeBackend_->syncCheats(cheatLookup_);
    // Report the BOOTED region — for region-free systems fall back to
    // whatever the analyzer said (usually empty).
    romRegion_ = region.empty() ? analysis.regionCsv : region;
    EMUHOST_LOGI("ROM loaded and powered on — region=%s", romRegion_.c_str());
    return 1;
}

bool EmulatorHost::flushSaves() {
    if (!romLoaded_ || !activeBackend_ || savePrefix_.empty()) return false;
    // Engine save() writes battery memory back into its media first.
    activeBackend_->syncSave();
    FileSaveMediaIO saves{savePrefix_};
    return activeBackend_->collectSaveMedia(saves);
}

// --- emulation ---------------------------------------------------------------

void EmulatorHost::rewindRun() {
    auto& rw = rewind_;
    if (!rw.enabled) return;

    if (!rw.rewinding) {
        if (++rw.counter < rw.frequency) return;
        rw.counter = 0;
        if (rw.history.size() >= rw.length) rw.history.erase(rw.history.begin());
        std::vector<uint8_t> snapshot;
        if (activeBackend_->serialize(snapshot)) rw.history.push_back(std::move(snapshot));
        return;
    }

    if (rw.history.empty()) {
        rw.rewinding = false;
        rw.counter = 0;
        return;
    }
    if (++rw.counter < std::max(1u, rw.frequency / 5)) return;
    rw.counter = 0;
    auto snapshot = std::move(rw.history.back());
    rw.history.pop_back();
    activeBackend_->unserialize(snapshot.data(), snapshot.size());
    if (rw.history.empty()) rw.rewinding = false;
}

bool EmulatorHost::tick() {
    if (!romLoaded_ || !activeBackend_) return false;

    // Consume pending controller changes on the emulation thread (bridge
    // threads only stored them + flagged). A device (re)connect rebuilds the
    // bindings (which re-applies the remap); a lone remap just recomputes
    // bits. Runs even while paused; the sampling side is this same thread.
    if (deviceDirty_.exchange(false, std::memory_order_relaxed)) {
        rebindPorts();
        inputRemapDirty_.store(false, std::memory_order_relaxed);
    } else if (inputRemapDirty_.exchange(false, std::memory_order_relaxed)) {
        applyInputRemap();
    }

    if (paused_.load(std::memory_order_relaxed)) return false;

    // DRC gates off during fast-forward, porting desktop's FastForwardOn →
    // audio.setDynamic(false): at 4× the resampler must not steer production
    // toward the DAC clock.
    if (dynamicRateControl_.load(std::memory_order_relaxed) &&
        !fastForwardActive_.load(std::memory_order_relaxed)) {
        double fill;
        {
            std::lock_guard<std::mutex> lock(audioMutex_);
            fill = (double)audioRing_.size() / kAudioCap;
        }
        activeBackend_->applyRateControl(fill);
    }

    // A drop or a starved read is a discontinuity the speaker plays: nonzero
    // counts implicate pacing, zero counts implicate engine sample content.
    if (++audioStatTicks_ >= 600) {
        audioStatTicks_ = 0;
        uint64_t dropped   = audioDropped_.exchange(0, std::memory_order_relaxed);
        uint64_t underruns = audioUnderruns_.exchange(0, std::memory_order_relaxed);
        if (dropped || underruns) {
            EMUHOST_LOGI("audio ring (last ~10s): dropped=%llu floats, empty reads=%llu",
                         (unsigned long long)dropped, (unsigned long long)underruns);
        }
    }

    rewindRun();

    // Desktop-ares run-ahead: a one-frame preview that reduces perceived
    // input latency at 2× emulation cost.
    const bool runAhead = runAheadEnabled_ &&
        !rewind_.rewinding &&
        !fastForwardActive_.load(std::memory_order_relaxed);
    if (!runAhead) {
        activeBackend_->tick(false);
    } else {
        activeBackend_->tick(true);
        runAheadScratch_.clear();
        activeBackend_->serialize(runAheadScratch_);
        activeBackend_->tick(false);
        activeBackend_->unserialize(runAheadScratch_.data(), runAheadScratch_.size());
    }
    return true;
}

void EmulatorHost::pause()  { paused_.store(true,  std::memory_order_relaxed); }
void EmulatorHost::resume() { paused_.store(false, std::memory_order_relaxed); }

// --- input --------------------------------------------------------------------

void EmulatorHost::setInput(int port, uint32_t bits) {
    if (port < 1 || port > kMaxPorts) return;
    hwMask_[port - 1].store(bits, std::memory_order_relaxed);
}

uint32_t EmulatorHost::combinedInput(int port) const {
    if (port < 1 || port > kMaxPorts) return 0;
    int i = port - 1;
    return hwMask_[i].load(std::memory_order_relaxed)
         | swMask_[i].load(std::memory_order_relaxed);
}

std::vector<PortBinding> EmulatorHost::resolveBindings() {
    std::vector<PortBinding> out;
    if (!stagedSystem_) return out;
    auto& sys = *stagedSystem_;

    if (sys.ports == 0) {
        // Built-in controls — always present on logical port 1.
        out.push_back({0, "", false, {{1, ""}}});
        return out;
    }

    // A multitap expands to consecutive LOGICAL numbers (port 2 → 2,3,4,5).
    int logical = 1;
    for (int p = 1; p <= sys.ports && logical <= kMaxPorts; p++) {
        std::string name;
        {
            std::lock_guard<std::mutex> lock(deviceMutex_);
            name = connectedDevice_[p - 1];
        }
        // Port 1 defaults to the system's own controller when nothing is
        // registered — the auto-connect rule every read path shares.
        name = SystemCatalog::effectiveDeviceName(sys, p, name);

        PortBinding binding{p, name, false, {}};
        if (name.empty()) {
            out.push_back(binding);   // explicit disconnect
            logical += 1;
            continue;
        }
        if (SystemCatalog::isMultitap(sys, name)) {
            binding.multitap = true;
            int block = SystemCatalog::portBlock(sys, name);
            for (int i = 1; i <= block && logical <= kMaxPorts; i++, logical++) {
                binding.logical.push_back({logical, "Gamepad"});
            }
        } else {
            binding.logical.push_back({logical, name});
            logical += 1;
        }
        out.push_back(binding);
    }
    return out;
}

void EmulatorHost::rebindPorts() {
    if (!activeBackend_ || !systemLoaded_) return;
    buttonTable_.clear();
    axisTable_.clear();
    activeBackend_->bindPorts(resolveBindings(), *this);
    applyInputRemap();
}

void EmulatorHost::applyInputRemap() {
    std::lock_guard<std::mutex> lock(inputRemapMutex_);
    for (auto& entry : buttonTable_) {
        entry.bit = entry.defaultBit;
        auto pit = inputRemap_.find(entry.port);
        if (pit == inputRemap_.end()) continue;
        auto it = pit->second.find(entry.name);
        if (it != pit->second.end()) entry.bit = it->second;
    }
}

bool EmulatorHost::connectedDescriptor(int port, SystemCatalog::DeviceDescriptor& out) {
    // Resolved against the STAGED system: descriptors answer from staging
    // on, before any boot.
    if (!stagedSystem_) return false;
    auto& sys = *stagedSystem_;
    if (sys.ports == 0) { out.buttons = sys.buttons; out.axes.clear(); return true; }

    std::string name;
    {
        std::lock_guard<std::mutex> lock(deviceMutex_);
        int logical = 1;
        for (int p = 1; p <= sys.ports && logical <= kMaxPorts; p++) {
            auto dev = SystemCatalog::effectiveDeviceName(sys, p, connectedDevice_[p - 1]);
            int block = SystemCatalog::portBlock(sys, dev);
            for (int i = 0; i < block && logical <= kMaxPorts; i++, logical++) {
                if (logical == port) {
                    name = SystemCatalog::isMultitap(sys, dev) ? std::string("Gamepad") : dev;
                }
            }
            if (!name.empty()) break;
        }
    }
    if (name.empty()) return false;
    return SystemCatalog::resolveDevice(sys, name, out);
}

std::string EmulatorHost::connectDevice(const std::string& systemId, int port,
                                        const std::string& device) {
    // Validate against the catalog + backend availability by id (static
    // data) so we never race an in-flight staging on the emulation thread.
    auto* sys = SystemCatalog::find(systemId);
    if (!sys || !Backends::forSystem(systemId)) return "SYSTEM_NOT_LOADED";

    // Built-in-controls systems have no ports to plug into — the controls
    // are always present, so a connect is a harmless no-op.
    if (sys->ports == 0) return "";

    int maxPort = std::min(sys->ports, kMaxPorts);
    if (port < 1 || port > maxPort) return "INVALID_PARAMETERS";

    SystemCatalog::DeviceDescriptor desc;
    if (!device.empty() && !SystemCatalog::resolveDevice(*sys, device, desc))
        return "UNSUPPORTED_DEVICE";

    {
        std::lock_guard<std::mutex> lock(deviceMutex_);
        connectedDevice_[port - 1] = device;
    }
    {
        // Reset the aimAt shadow cursor to center, matching a fresh
        // device. Telescoping deltas keep it in sync even if aimAt runs
        // before the deferred rebind.
        std::lock_guard<std::mutex> lock(axisMutex_);
        aimX_[port - 1] = kAimW / 2;
        aimY_[port - 1] = kAimH / 2;
    }
    deviceDirty_.store(true, std::memory_order_relaxed);
    return "";
}

std::vector<int> EmulatorHost::devicePorts(const std::string& systemId, int physical) {
    std::vector<int> out;
    auto* sys = SystemCatalog::find(systemId);
    if (!sys || !Backends::forSystem(systemId)) return out;

    int logical = 1;
    for (int p = 1; p <= sys->ports && logical <= kMaxPorts; p++) {
        std::string name;
        {
            std::lock_guard<std::mutex> lock(deviceMutex_);
            name = connectedDevice_[p - 1];
        }
        int block = SystemCatalog::portBlock(*sys, name);
        for (int i = 0; i < block && logical <= kMaxPorts; i++, logical++) {
            if (p == physical) out.push_back(logical);
        }
    }
    return out;
}

std::string EmulatorHost::pressButton(int port, const std::string& name, bool down) {
    if (!stagedSystem_) return "SYSTEM_NOT_LOADED";
    if (port < 1 || port > kMaxPorts) return "INVALID_PARAMETERS";

    SystemCatalog::DeviceDescriptor desc;
    uint32_t bit;
    if (!connectedDescriptor(port, desc) || !bitForButtonName(desc, name, bit))
        return "UNKNOWN_BUTTON:" + name;

    int i = port - 1;
    if (down) {
        swMask_[i].fetch_or(bit, std::memory_order_relaxed);
        unsampledPress_[i].fetch_or(bit, std::memory_order_relaxed);
        // A re-press cancels any release still queued from the previous tap.
        deferredRelease_[i].fetch_and(~bit, std::memory_order_relaxed);
    } else if (unsampledPress_[i].load(std::memory_order_relaxed) & bit) {
        // Press not yet sampled by the engine — defer the release one poll
        // so the tap isn't lost (see sampleButton's latch consume).
        deferredRelease_[i].fetch_or(bit, std::memory_order_relaxed);
    } else {
        swMask_[i].fetch_and(~bit, std::memory_order_relaxed);
    }
    return "";
}

std::string EmulatorHost::setAxis(int port, const std::string& name, int value) {
    if (!stagedSystem_) return "SYSTEM_NOT_LOADED";
    if (port < 1 || port > kMaxPorts) return "INVALID_PARAMETERS";

    SystemCatalog::DeviceDescriptor desc;
    if (!connectedDescriptor(port, desc) ||
        std::find(desc.axes.begin(), desc.axes.end(), name) == desc.axes.end())
        return "INVALID_PARAMETERS";

    std::lock_guard<std::mutex> lock(axisMutex_);
    axisAccum_[port - 1][name] += value;
    return "";
}

std::string EmulatorHost::aimAt(int port, float nx, float ny) {
    if (!stagedSystem_) return "SYSTEM_NOT_LOADED";
    if (port < 1 || port > kMaxPorts) return "INVALID_PARAMETERS";

    SystemCatalog::DeviceDescriptor desc;
    auto has = [&](const char* a) {
        return std::find(desc.axes.begin(), desc.axes.end(), a) != desc.axes.end();
    };
    if (!connectedDescriptor(port, desc) || !has("X") || !has("Y"))
        return "INVALID_PARAMETERS";

    float cx = nx < 0 ? 0 : (nx > 1 ? 1 : nx);
    float cy = ny < 0 ? 0 : (ny > 1 ? 1 : ny);
    int tx = (int)(cx * kAimW);
    int ty = (int)(cy * kAimH);

    std::lock_guard<std::mutex> lock(axisMutex_);
    axisAccum_[port - 1]["X"] += tx - aimX_[port - 1];
    axisAccum_[port - 1]["Y"] += ty - aimY_[port - 1];
    aimX_[port - 1] = tx;   // target is in-bounds (0..W/0..H)
    aimY_[port - 1] = ty;
    return "";
}

std::string EmulatorHost::setInputMapping(int port,
                                          const std::vector<std::string>& emulated,
                                          const std::vector<std::string>& source) {
    if (!stagedSystem_) return "SYSTEM_NOT_LOADED";
    if (port < 1 || port > kMaxPorts) return "INVALID_PARAMETERS";
    if (emulated.size() != source.size()) return "INVALID_PARAMETERS";

    // Remap names belong to the device at this logical port (multitap-aware).
    SystemCatalog::DeviceDescriptor desc;
    if (!connectedDescriptor(port, desc))
        return "INVALID_PARAMETERS";   // no controller registered on this port

    // Resolve+validate the whole batch before mutating any state, so a bad
    // entry leaves the existing remap untouched.
    std::unordered_map<std::string, uint32_t> resolved;
    for (size_t i = 0; i < emulated.size(); i++) {
        uint32_t emuBit, srcBit;
        if (!bitForButtonName(desc, emulated[i], emuBit))
            return "UNKNOWN_BUTTON:" + emulated[i];
        if (!bitForButtonName(desc, source[i], srcBit))
            return "UNKNOWN_BUTTON:" + source[i];
        resolved[toLower(emulated[i])] = srcBit;
    }

    {
        std::lock_guard<std::mutex> lock(inputRemapMutex_);
        if (emulated.empty()) {
            inputRemap_.erase(port);
        } else {
            auto& portMap = inputRemap_[port];
            for (auto& [name, bit] : resolved) portMap[name] = bit;
        }
    }
    inputRemapDirty_.store(true, std::memory_order_relaxed);
    return "";
}

int EmulatorHost::getButtonBit(int port, const std::string& name) {
    auto lname = toLower(name);
    for (auto& entry : buttonTable_) {
        if (entry.port != port) continue;
        if (entry.name == lname) return (int)entry.bit;
    }
    return -1;
}

int EmulatorHost::getAxisAccum(int port, const std::string& name) {
    if (port < 1 || port > kMaxPorts) return 0;
    std::lock_guard<std::mutex> lock(axisMutex_);
    auto& m = axisAccum_[port - 1];
    auto it = m.find(name);
    return it == m.end() ? 0 : (int)it->second;
}

std::string EmulatorHost::pressedButtons(int port) {
    if (!stagedSystem_ || port < 1 || port > kMaxPorts) return "";

    SystemCatalog::DeviceDescriptor desc;
    if (!connectedDescriptor(port, desc)) return "";

    int i = port - 1;
    uint32_t mask = hwMask_[i].load(std::memory_order_relaxed)
                  | swMask_[i].load(std::memory_order_relaxed);

    std::string out;
    for (auto& name : orderedButtons(desc)) {
        uint32_t bit;
        if (!bitForButtonName(desc, name, bit) || !(mask & bit)) continue;
        if (!out.empty()) out += ",";
        out += name;
    }
    return out;
}

std::string EmulatorHost::portsJson() {
    // Catalog data + registrations — available from staging on, no booted
    // engine required.
    if (!stagedSystem_) return "[]";
    auto& sys = *stagedSystem_;
    auto supported = jsonStringArray(SystemCatalog::supportedDevices(sys));

    if (sys.ports == 0) {   // built-in controls — always present
        SystemCatalog::DeviceDescriptor desc;
        desc.buttons = sys.buttons;
        return "[{\"port\":1,\"device\":null,\"buttons\":"
             + jsonStringArray(orderedButtons(desc))
             + ",\"axes\":[],\"supported\":" + supported + "}]";
    }

    std::string json = "[";
    auto emit = [&](int lport, const std::string& devName,
                    const SystemCatalog::DeviceDescriptor* d) {
        if (json.size() > 1) json += ",";
        json += "{\"port\":" + std::to_string(lport)
              + ",\"device\":" + (devName.empty() ? "null" : "\"" + devName + "\"")
              + ",\"buttons\":" + (d ? jsonStringArray(orderedButtons(*d)) : "[]")
              + ",\"axes\":" + (d ? jsonStringArray(d->axes) : "[]")
              + ",\"supported\":" + supported + "}";
    };

    int logical = 1;
    for (int p = 1; p <= sys.ports && logical <= kMaxPorts; p++) {
        std::string name;
        {
            std::lock_guard<std::mutex> lock(deviceMutex_);
            name = connectedDevice_[p - 1];
        }
        name = SystemCatalog::effectiveDeviceName(sys, p, name);
        if (SystemCatalog::isMultitap(sys, name)) {   // fans out to gamepads
            SystemCatalog::DeviceDescriptor gp;
            gp.buttons = sys.buttons;
            int block = SystemCatalog::portBlock(sys, name);
            for (int i = 0; i < block && logical <= kMaxPorts; i++, logical++)
                emit(logical, "Gamepad", &gp);
        } else {
            SystemCatalog::DeviceDescriptor desc;
            bool ok = !name.empty() && SystemCatalog::resolveDevice(sys, name, desc);
            emit(logical, ok ? name : std::string(), ok ? &desc : nullptr);
            logical++;
        }
    }
    return json + "]";
}

// --- AV options ---------------------------------------------------------------

void EmulatorHost::setAudio(float volume, float balance) {
    volume_.store(std::clamp(volume, 0.0f, 1.0f), std::memory_order_relaxed);
    balance_.store(std::clamp(balance, -1.0f, 1.0f), std::memory_order_relaxed);
}

void EmulatorHost::setRawAudio(bool raw) {
    rawAudio_.store(raw, std::memory_order_relaxed);
}

void EmulatorHost::setVideo(float luminance, float saturation, float gamma,
                            bool colorBleed, bool overscan) {
    if (!systemLoaded_ || !activeBackend_) return;
    overscan_ = overscan;
    activeBackend_->setVideoSettings(luminance, saturation, gamma, colorBleed, overscan);
}

void EmulatorHost::setCoreBoolean(const std::string& key, bool value) {
    if (!systemLoaded_ || !activeBackend_) return;
    activeBackend_->applyRuntimeToggle(key, value);
}

int EmulatorHost::coreBoolean(const std::string& key) {
    if (!systemLoaded_ || !activeBackend_) return -1;
    return activeBackend_->readRuntimeToggle(key);
}

// --- video out ------------------------------------------------------------------

bool EmulatorHost::withDirtyFrame(
    const std::function<void(const uint32_t*, uint32_t, uint32_t)>& fn) {
    std::lock_guard<std::mutex> lock(frameMutex_);
    if (!frameDirty_) return false;
    fn(frameBuffer_.data(), frameWidth_, frameHeight_);
    frameDirty_ = false;
    return true;
}

bool EmulatorHost::copyLatestFrame(uint32_t* out, size_t capacity,
                                   uint32_t* width, uint32_t* height) {
    std::lock_guard<std::mutex> lock(frameMutex_);
    if (frameBuffer_.empty()) {
        if (width)  *width  = 0;
        if (height) *height = 0;
        return false;
    }
    if (width)  *width  = frameWidth_;
    if (height) *height = frameHeight_;
    if (out && capacity > 0) {
        size_t count = std::min(capacity, (size_t)frameWidth_ * frameHeight_);
        std::memcpy(out, frameBuffer_.data(), count * sizeof(uint32_t));
    }
    return true;
}

uint32_t EmulatorHost::frameWidth() const {
    std::lock_guard<std::mutex> lock(frameMutex_);
    return frameWidth_;
}

uint32_t EmulatorHost::frameHeight() const {
    std::lock_guard<std::mutex> lock(frameMutex_);
    return frameHeight_;
}

void EmulatorHost::videoGeometry(double out[7]) {
    out[0] = 0; out[1] = 0; out[2] = 1; out[3] = 1;
    out[4] = 1; out[5] = 1; out[6] = 0;
    std::lock_guard<std::mutex> lock(frameMutex_);
    out[0] = geometry_.width;
    out[1] = geometry_.height;
    out[2] = geometry_.scaleX;
    out[3] = geometry_.scaleY;
    out[4] = geometry_.aspectX;
    out[5] = geometry_.aspectY;
    out[6] = (double)geometry_.rotation;
}

double EmulatorHost::refreshRateHint() const {
    return refreshRateHint_.load(std::memory_order_relaxed);
}

// --- audio out -------------------------------------------------------------------

size_t EmulatorHost::readAudio(float* out, size_t capacity) {
    if (!out || capacity == 0) return 0;
    std::lock_guard<std::mutex> lock(audioMutex_);
    if (audioRing_.empty()) {
        audioUnderruns_.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }
    size_t count = std::min(capacity, audioRing_.size());
    std::memcpy(out, audioRing_.data(), count * sizeof(float));
    audioRing_.erase(audioRing_.begin(), audioRing_.begin() + (ptrdiff_t)count);
    return count;
}

// --- save states -----------------------------------------------------------------

bool EmulatorHost::stateSave(const std::string& path) {
    if (!romLoaded_ || !activeBackend_) return false;

    std::vector<uint8_t> payload;
    if (!activeBackend_->serialize(payload)) return false;

    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
        EMUHOST_LOGE("stateSave: cannot open %s", path.c_str());
        return false;
    }
    const std::string name = activeBackend_->name();
    bool ok = std::fwrite(kStateMagic, 1, sizeof(kStateMagic), f) == sizeof(kStateMagic);
    uint8_t nameLen = (uint8_t)std::min<size_t>(name.size(), 255);
    ok = ok && std::fwrite(&nameLen, 1, 1, f) == 1;
    ok = ok && std::fwrite(name.data(), 1, nameLen, f) == nameLen;
    ok = ok && std::fwrite(payload.data(), 1, payload.size(), f) == payload.size();
    std::fclose(f);
    if (ok) EMUHOST_LOGI("state saved: %s (%zu bytes)", path.c_str(), payload.size());
    return ok;
}

bool EmulatorHost::stateLoad(const std::string& path) {
    if (!romLoaded_ || !activeBackend_) return false;

    auto data = readFile(path);
    if (data.empty()) {
        EMUHOST_LOGE("stateLoad: file not found: %s", path.c_str());
        return false;
    }

    const uint8_t* payload = data.data();
    size_t size = data.size();
    if (size > sizeof(kStateMagic) + 1 &&
        std::memcmp(data.data(), kStateMagic, sizeof(kStateMagic)) == 0) {
        uint8_t nameLen = data[sizeof(kStateMagic)];
        size_t headerSize = sizeof(kStateMagic) + 1 + nameLen;
        if (size <= headerSize) return false;
        std::string savedBy((const char*)data.data() + sizeof(kStateMagic) + 1, nameLen);
        if (savedBy != activeBackend_->name()) {
            // Engine states are not portable between engines — fail loudly
            // instead of feeding one engine another's bytes.
            EMUHOST_LOGE("stateLoad: state saved by backend '%s', active backend is '%s'",
                         savedBy.c_str(), activeBackend_->name());
            return false;
        }
        payload += headerSize;
        size -= headerSize;
    }
    // No magic = an untagged legacy file: raw ares payload.

    bool ok = activeBackend_->unserialize(payload, size);
    EMUHOST_LOGI("state loaded: %s — %s", path.c_str(), ok ? "ok" : "failed");
    return ok;
}

// --- memory window ------------------------------------------------------------------

int EmulatorHost::readMemory(uint32_t address, uint8_t* out, uint32_t length) {
    if (!romLoaded_ || !activeBackend_ || !activeSystem_ || !out || length == 0) return -1;

    if (address < activeSystem_->memBase) return -1;
    uint32_t offset = address - activeSystem_->memBase;
    if (offset + length > activeSystem_->memSize) return -1;

    return activeBackend_->readMemory(offset, out, length);
}

void EmulatorHost::writeMemory(uint32_t address, const uint8_t* data, uint32_t length) {
    if (!romLoaded_ || !activeBackend_ || !activeSystem_ || !data || length == 0) return;

    if (address < activeSystem_->memBase) return;
    uint32_t offset = address - activeSystem_->memBase;
    if (offset + length > activeSystem_->memSize) return;

    activeBackend_->writeMemory(offset, data, length);
}

// --- cheats -----------------------------------------------------------------------

void EmulatorHost::rebuildCheatLookup() {
    cheatLookup_.clear();
    for (auto& [code, pairs] : cheats_) {
        for (auto& [addr, value] : pairs) cheatLookup_[addr] = value;
    }
    // Push-style engines mirror the table into their own cheat slots.
    if (activeBackend_ && systemLoaded_) activeBackend_->syncCheats(cheatLookup_);
}

bool EmulatorHost::addCheat(const std::string& code) {
    auto pairs = CheatParse::parse(code);
    if (pairs.empty()) {
        EMUHOST_LOGE("addCheat: no valid ADDR:VALUE pairs in '%s'", code.c_str());
        return false;
    }

    cheats_[code] = std::move(pairs);
    rebuildCheatLookup();
    EMUHOST_LOGI("cheat added: '%s' (%zu total)", code.c_str(), cheats_.size());
    return true;
}

bool EmulatorHost::removeCheat(const std::string& code) {
    bool removed = cheats_.erase(code) > 0;
    if (removed) rebuildCheatLookup();
    return removed;
}

void EmulatorHost::clearCheats() {
    cheats_.clear();
    rebuildCheatLookup();
}

// --- rewind / run-ahead / pacing ------------------------------------------------------

void EmulatorHost::configureRewind(bool enabled, int bufferSeconds) {
    auto& rw = rewind_;
    rw.enabled = enabled;
    rw.length  = bufferSeconds > 0 ? (uint32_t)bufferSeconds * 6u : 100u;
    if (!enabled) {
        rw.rewinding = false;
        rw.counter = 0;
        rw.history.clear();
    }
}

int EmulatorHost::toggleRewind() {
    if (!rewind_.enabled) return -1;
    rewind_.rewinding = !rewind_.rewinding;
    rewind_.counter = 0;
    return rewind_.rewinding ? 1 : 0;
}

void EmulatorHost::setRunAhead(bool enabled) { runAheadEnabled_ = enabled; }

void EmulatorHost::setFastForward(bool active) {
    fastForwardActive_.store(active, std::memory_order_relaxed);
}

void EmulatorHost::setDynamicRateControl(bool enabled) {
    dynamicRateControl_.store(enabled, std::memory_order_relaxed);
}

// --- rumble ------------------------------------------------------------------------

void EmulatorHost::setRumbleEnabled(bool enabled) {
    rumbleEnabled_.store(enabled, std::memory_order_relaxed);
    if (!enabled) rumbleState_.store(0, std::memory_order_relaxed);
}

uint32_t EmulatorHost::rumbleState() const {
    return rumbleState_.load(std::memory_order_relaxed);
}

// --- capabilities --------------------------------------------------------------------

bool EmulatorHost::videoSettingsSupported() const {
    auto* backend = activeBackend_ ? activeBackend_ : stagedBackend_;
    auto* system  = activeBackend_ ? activeSystem_  : stagedSystem_;
    if (!backend || !system) return true;   // nothing staged: nothing to reject yet
    return backend->capabilities(system->id).videoSettings;
}

bool EmulatorHost::toggleSupported(const std::string& key) const {
    auto* backend = activeBackend_ ? activeBackend_ : stagedBackend_;
    auto* system  = activeBackend_ ? activeSystem_  : stagedSystem_;
    if (!backend || !system) return true;
    for (auto& option : backend->capabilities(system->id).options) {
        if (option.stage == OptionInfo::Stage::Runtime && option.key == key) return true;
    }
    return false;
}

std::string EmulatorHost::backendName() const {
    if (activeBackend_) return activeBackend_->name();
    if (stagedBackend_) return stagedBackend_->name();
    return "";
}

std::string EmulatorHost::setEngineOption(const std::string& key, const std::string& value,
                                          bool staged) {
    auto* backend = staged ? (stagedBackend_ ? stagedBackend_ : activeBackend_)
                           : activeBackend_;
    if (!backend) return staged ? "no system is staged" : "no system is loaded";
    return backend->setEngineOption(key, value, staged);
}

namespace {
// Core-authored strings (option keys/choices) can carry anything.
auto jsonEscape(const std::string& raw) -> std::string {
    std::string out;
    for (char c : raw) {
        if (c == '"' || c == '\\') { out += '\\'; out += c; }
        else if ((unsigned char)c < 0x20) { out += ' '; }
        else out += c;
    }
    return out;
}
} // namespace

std::string EmulatorHost::engineOptionsJson() const {
    auto* backend = activeBackend_ ? activeBackend_ : stagedBackend_;
    std::string json = "[";
    if (backend) {
        for (auto& option : backend->engineOptions()) {
            if (json.size() > 1) json += ",";
            json += "{\"key\":\"" + jsonEscape(option.key) + "\",\"choices\":[";
            for (size_t i = 0; i < option.values.size(); i++) {
                if (i) json += ",";
                json += "\"" + jsonEscape(option.values[i]) + "\"";
            }
            json += "],\"default\":\""
                 + jsonEscape(option.values.empty() ? "" : option.values.front())
                 + "\",\"current\":\"" + jsonEscape(option.current) + "\"}";
        }
    }
    return json + "]";
}

std::string EmulatorHost::backendsJson() {
    // Root "engines" lists every REGISTERED backend, claimant or not — the
    // only observable proof the libretro loader (which claims no system)
    // actually linked/loaded. Bridges treat root keys as systems and skip
    // the array value safely.
    std::string json = "{\"engines\":[";
    for (auto& name : Backends::names()) {
        if (json.back() != '[') json += ",";
        json += "\"" + name + "\"";
    }
    json += "]";
    for (auto& id : Backends::availableSystems()) {
        std::string claimants;
        for (auto& name : Backends::names()) {
            auto* backend = Backends::byName(name);
            if (!backend) continue;
            auto ids = backend->systems();
            if (std::find(ids.begin(), ids.end(), id) == ids.end()) continue;
            if (!claimants.empty()) claimants += ",";
            claimants += "\"" + name + "\"";
        }
        json += ",\"" + id + "\":{\"backends\":[" + claimants + "]}";
    }
    return json + "}";
}

// --- metadata ----------------------------------------------------------------------

std::string EmulatorHost::region() const { return romRegion_; }

std::vector<std::string> EmulatorHost::systemExtensionsFor(const std::string& systemId) {
    auto* sys = SystemCatalog::find(systemId);
    if (!sys || !Backends::forSystem(systemId)) return {};
    return sys->extensions;
}

// --- HostPort (backend-facing) -------------------------------------------------------

int EmulatorHost::buttonHandle(int logicalPort, const std::string& button) {
    if (logicalPort < 1 || logicalPort > kMaxPorts) return -1;
    SystemCatalog::DeviceDescriptor desc;
    if (!connectedDescriptor(logicalPort, desc)) return -1;
    uint32_t bit;
    if (!bitForButtonName(desc, button, bit)) return -1;
    buttonTable_.push_back({logicalPort, toLower(button), bit, bit});
    return (int)buttonTable_.size() - 1;
}

bool EmulatorHost::sampleButton(int handle) {
    if (handle < 0 || (size_t)handle >= buttonTable_.size()) return false;
    auto& entry = buttonTable_[(size_t)handle];
    int i = entry.port - 1;
    uint32_t mask = hwMask_[i].load(std::memory_order_relaxed)
                  | swMask_[i].load(std::memory_order_relaxed);
    bool value = mask & entry.bit;
    // Press latch: this bit has now been sampled once; apply a release that
    // raced in before the press was ever seen.
    if (unsampledPress_[i].load(std::memory_order_relaxed) & entry.bit) {
        unsampledPress_[i].fetch_and(~entry.bit, std::memory_order_relaxed);
        if (deferredRelease_[i].fetch_and(~entry.bit, std::memory_order_relaxed) & entry.bit)
            swMask_[i].fetch_and(~entry.bit, std::memory_order_relaxed);
    }
    return value;
}

int EmulatorHost::axisHandle(int logicalPort, const std::string& axis) {
    if (logicalPort < 1 || logicalPort > kMaxPorts) return -1;
    SystemCatalog::DeviceDescriptor desc;
    if (!connectedDescriptor(logicalPort, desc)) return -1;
    if (std::find(desc.axes.begin(), desc.axes.end(), axis) == desc.axes.end()) return -1;
    axisTable_.push_back({logicalPort, axis});
    return (int)axisTable_.size() - 1;
}

int32_t EmulatorHost::consumeAxisDelta(int handle) {
    if (handle < 0 || (size_t)handle >= axisTable_.size()) return 0;
    auto& entry = axisTable_[(size_t)handle];
    std::lock_guard<std::mutex> lock(axisMutex_);
    auto& acc = axisAccum_[entry.port - 1][entry.name];
    int32_t value = acc;
    acc = 0;   // consume: a relative delta applies once per poll
    return value;
}

void EmulatorHost::captureFrame(std::vector<uint32_t>& out) {
    std::lock_guard<std::mutex> lock(frameMutex_);
    out = frameBuffer_;
}

uint64_t EmulatorHost::frameChecksum() {
    std::lock_guard<std::mutex> lock(frameMutex_);
    uint64_t h = 1469598103934665603ull;
    for (uint32_t px : frameBuffer_) {
        h ^= px;
        h *= 1099511628211ull;
    }
    return h;
}

void EmulatorHost::pushFrame(const uint32_t* argb, uint32_t width, uint32_t height,
                             uint32_t strideWords, const FrameGeometry& geometry) {
    // Engines deliver frames from their own worker threads — buffer under
    // the frame lock; the emulation/GL thread uploads on its next pass.
    std::lock_guard<std::mutex> lock(frameMutex_);
    frameWidth_  = width;
    frameHeight_ = height;
    geometry_    = geometry;
    frameBuffer_.resize((size_t)width * height);
    if (strideWords == width) {
        std::memcpy(frameBuffer_.data(), argb, (size_t)width * height * sizeof(uint32_t));
    } else {
        for (uint32_t y = 0; y < height; y++) {
            std::memcpy(&frameBuffer_[(size_t)y * width],
                        argb + (size_t)y * strideWords, width * sizeof(uint32_t));
        }
    }
    // Boot-skip phase 2 reads these frames for its checksum; never presented.
    frameDirty_ = !bootSkipDiscard_.load(std::memory_order_relaxed)
                  && !bootSkipVideoHold_.load(std::memory_order_relaxed);
}

// 60 Hz: below this the GB path measures 30 dB hotter than the reference.
namespace {

struct Highpass {
    double b0, b1, b2, a1, a2;
};

const Highpass& gbHighpass() {
    static const Highpass hp = [] {
        constexpr double kCorner = 60.0, kRate = 48000.0, kQ = 0.70710678;
        const double w0 = 2.0 * 3.14159265358979323846 * kCorner / kRate;
        const double c = std::cos(w0), s = std::sin(w0), alpha = s / (2.0 * kQ);
        const double a0 = 1.0 + alpha;
        return Highpass{(1.0 + c) / 2.0 / a0, -(1.0 + c) / a0, (1.0 + c) / 2.0 / a0,
                        -2.0 * c / a0, (1.0 - alpha) / a0};
    }();
    return hp;
}

}  // namespace

void EmulatorHost::pushAudioFrame(double left, double right) {
    if (bootSkipDiscard_.load(std::memory_order_relaxed)) {
        if (!bootSkipDcPrimed_) {
            bootSkipDcL_ = left;
            bootSkipDcR_ = right;
            bootSkipDcPrimed_ = true;
        }
        bootSkipDcL_ += 0.004 * (left - bootSkipDcL_);
        bootSkipDcR_ += 0.004 * (right - bootSkipDcR_);
        const double m = std::max(std::abs(left - bootSkipDcL_),
                                  std::abs(right - bootSkipDcR_));
        if (m > bootSkipAudioPeak_) bootSkipAudioPeak_ = m;
        bootSkipTickAudio_.push_back((float)left);
        bootSkipTickAudio_.push_back((float)right);
        return;
    }

    // Ahead of volume: a volume change would otherwise step the filter's input.
    if (audioHighpass_ && !rawAudio_.load(std::memory_order_relaxed)) {
        const Highpass& hp = gbHighpass();
        const double in[2] = {left, right};
        if (!hpPrimed_) {
            // Seeding output history too steps the ring by the whole pedestal.
            for (int ch = 0; ch < 2; ++ch) {
                hpX1_[ch] = hpX2_[ch] = in[ch];
                hpY1_[ch] = hpY2_[ch] = 0.0;
            }
            hpPrimed_ = true;
        }
        double out[2];
        for (int ch = 0; ch < 2; ++ch) {
            out[ch] = hp.b0 * in[ch] + hp.b1 * hpX1_[ch] + hp.b2 * hpX2_[ch]
                      - hp.a1 * hpY1_[ch] - hp.a2 * hpY2_[ch];
            hpX2_[ch] = hpX1_[ch];
            hpX1_[ch] = in[ch];
            hpY2_[ch] = hpY1_[ch];
            hpY1_[ch] = out[ch];
        }
        left = out[0];
        right = out[1];
    }

    float l = (float)std::max(-1.0, std::min(+1.0, left));
    float r = (float)std::max(-1.0, std::min(+1.0, right));

    const float volume  = volume_.load(std::memory_order_relaxed);
    const float balance = balance_.load(std::memory_order_relaxed);
    l *= volume * (balance > 0.0f ? 1.0f - balance : 1.0f);
    r *= volume * (balance < 0.0f ? 1.0f + balance : 1.0f);

    std::lock_guard<std::mutex> lock(audioMutex_);
    audioRing_.push_back(l);
    audioRing_.push_back(r);
    if (audioRing_.size() > kAudioCap) {
        // Drop the oldest samples — carrying them would turn a burst of
        // emulation (startup, fast-forward) into permanent lag.
        audioDropped_.fetch_add(audioRing_.size() - kAudioCap,
                                std::memory_order_relaxed);
        audioRing_.erase(audioRing_.begin(),
                         audioRing_.begin() + (audioRing_.size() - kAudioCap));
    }
}

void EmulatorHost::setRefreshRateHint(double hz) {
    if (refreshRateHint_.load(std::memory_order_relaxed) != hz) {
        EMUHOST_LOGI("refresh rate hint changed to %f", hz);
    }
    refreshRateHint_.store(hz, std::memory_order_relaxed);
}

void EmulatorHost::setRumble(uint16_t strong, uint16_t weak) {
    const uint32_t state = rumbleEnabled_.load(std::memory_order_relaxed)
        ? (uint32_t)strong << 16 | weak
        : 0u;
    rumbleState_.store(state, std::memory_order_relaxed);
}

const std::unordered_map<uint32_t, uint32_t>* EmulatorHost::cheatTable() const {
    return &cheatLookup_;
}

} // namespace EmuHost
