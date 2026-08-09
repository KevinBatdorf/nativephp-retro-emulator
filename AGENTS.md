# AGENTS.md

Instructions and full reference for agents working in this repo or building
apps against it. The README is the short human pitch; this file is the docs.

## What this repo is

A NativePHP Mobile (v4 / EDGE) plugin that embeds retro consoles in Laravel
mobile apps. One engine-neutral C++ host (`native/host/`) owns all policy —
input latching, remaps, audio ring, cheats, rewind, run-ahead, saves, region
resolution — and engines plug in behind a seam (`native/host/backend.hpp`):

- **ares** (ISC) — always present, all five systems, accuracy-first.
- **SameBoy** (Expat) — bundled fast GB/GBC engine.
- **mGBA** (MPL-2.0) — bundled fast GBA engine.
- **libretro loader** — adopts any libretro core an app ships (bring your
  own; nothing bundled).

Kotlin (`resources/android/`) and Swift (`resources/ios/`) bridges expose the
host to PHP over the NativePHP bridge; `src/` is the typed PHP surface.

Systems: `fc` (NES), `sfc` (SNES), `gb`/`gbc`, `gba`, `md` (Genesis).

## Repo map

| Path | What lives there |
|---|---|
| `native/host/` | Engine-neutral host: seam (`backend.hpp`), registry, system catalog (button bits, extensions, memory windows), `emulator_host.cpp` |
| `native/backends/{ares,sameboy,mgba,libretro}/` | One adapter per engine |
| `native/cores/`, `native/pak/`, `native/firmware/` | ares per-system defs, pak builders, embedded firmware |
| `android/app/` | JNI (`emulator_jni.cpp`), CMake, instrumented tests, dev-harness `EmulatorActivity` |
| `ios/` | `emulator_api.{h,cpp}` C API, CMake, static-link anchors (`core_link.cpp`), SwiftPM test package (`ios/test/`) |
| `resources/android/`, `resources/ios/` | The bridge layers apps actually run (EmulatorCore/Renderer/Functions + elements) |
| `resources/android/jniLibs/` | Prebuilt `.so` set consumers install (committed; rebuild after any native change) |
| `src/` | PHP: Emulator, configs, enums, events, elements, artisan commands |
| `tests/` | Pest (includes drift scanners that parse native sources) |
| `scripts/` | Native build scripts |
| `ares/`, `sameboy/`, `mgba/` | Pinned engine submodules |
| `local/`, `vendor-engines/`, `.claude/` | Gitignored: ROMs, downloaded cores, plans — never commit contents |

## Build and test

```bash
# Native rebuild — REQUIRED after any native/ change; the committed
# prebuilts in resources/android/jniLibs and the xcframework are what
# consumers run. Instrumented tests compile from source and can go green
# while a stale prebuilt still crashes apps.
./scripts/build_android_libs.sh
./scripts/build_xcframework.sh

# PHP
./vendor/bin/pest                              # 152 tests, <1s

# Android instrumented (device attached; 103 tests)
cd android && ./gradlew connectedDebugAndroidTest --no-daemon

# iOS simulator (63 tests; needs build/RetroEmulator.xcframework)
cd ios/test && xcodebuild test -scheme RetroEmulatorTest-Package \
  -destination 'platform=iOS Simulator,name=iPhone 17 Pro Max,OS=26.5'
```

BYO-core tests stage cores into gitignored `android/app/src/androidTest/jniLibs/arm64-v8a/`
(SELinux only allows dlopen from installed APKs — never `/data/local/tmp`):

```bash
cp vendor-engines/cores/snes9x_libretro_android.so \
   android/app/src/androidTest/jniLibs/arm64-v8a/libsnes9x_libretro_android.so
```

Gates before a commit: Pest + full instrumented suite + iOS suite green; if
behavior is user-visible, verify on hardware with a real ROM and your eyes.

## PHP API

Everything hangs off a surface handle. Commands are fluent; queries return
values. Operational failures dispatch `EmulatorError` events; programmer
errors throw `EmulatorException` synchronously.

```php
$emu = Emulator::surface('main');   // binds the named <native:emulator>
```

### Boot and playback

| Call | Notes |
|---|---|
| `loadSystem(System\|string, SystemConfig\|array)` | Stages system + engine + options; synchronous validation |
| `loadRom(string\|array, ?string $savePath)` | Boots; call again to swap ROMs. Array form for slotted media: `['base' => $biosRom, 'slotA' => $cart, 'slotB' => …]` (SuFami Turbo, Satellaview) |
| `pause()` / `resume()` / `stop()` | |
| `setSpeed(float)` | 0.25–4.0 multiplier, 1.0 native |
| `fastForward(bool)` | |
| `toggleRewind()` | Needs `rewind: true` in config/configure; throws `REWIND_DISABLED` otherwise |
| `configure(array)` | Runtime merge: `speed`, `runAhead` (0\|1), `rewind`, `rewindBufferSeconds`, `engineOptions`; any other key throws `INVALID_PARAMETERS` (`pixelAccuracy` specifically throws `BOOT_ONLY_OPTION`) |
| `setSystemOptions(array)` | Per-system toggles (`deepBlackBoost`, `colorEmulation`, `interframeBlending`); enabling one the engine+system pair lacks throws `UNSUPPORTED_OPTION` — the legal set is `capabilities()['toggles']` |

### Save states and battery saves

`saveState($slot)` / `loadState($slot)` / `undoSaveState()` / `undoLoadState()`.
Battery saves persist under `loadRom(savePath:)`; flushed on ROM swap and stop.
States are engine-tagged — loading a state saved by a different engine errors
instead of corrupting.

### Memory

`readMemory($addr, $len)` (sync array of bytes), `readMemoryAsync` (→
`MemoryRead` event), `writeMemory($addr, bytes[])`, `watchMemory($addr, $len)`
(→ `MemoryChanged` on change), `unwatchMemory`, `clearMemoryWatches`.
Addresses are the system's work-RAM bus window (e.g. SNES `0x7E0000+`).

### Input

```php
$pad = $emu->connectDevice(1, Device::Gamepad);      // returns Controller
$pad->press(SfcButton::A)->release(SfcButton::A);    // or press('a')
$pad->pressed();                                     // ['a', 'start', …]
$pad->setButtons(['a' => true, 'b' => false]);       // batch
$pad->remap(['a' => 'b', 'b' => 'a']);               // in-game reads swapped
$mouse = $emu->connectDevice(1, Device::Mouse);
$mouse->setAxis('X', 5);                             // relative delta
$mouse->aimAt(0.5, 0.5);                             // absolute, normalized 0..1
$players = $emu->connectMultitap(2, Device::SuperMultitap);  // Controller[4]
$emu->getDevice(1); $emu->setRumble(true); $emu->ports();
Emulator::inputDevices();                            // OS-reported gamepads
```

Port 1 auto-connects the system's default pad at boot; `connectDevice` is for
everything else. Button bits are positional and identical across the Kotlin,
Swift, PHP (`Buttons\*Button` enums) and libretro (RetroPad) layers.

### Presentation and audio

`setVideo(luminance:, saturation:, gamma:, colorBleed:, overscan:, output:,
fixedScale:, aspectCorrection:)` — merge-per-key. `setVolume(0–100)`,
`setBalance(−100–100)`, `setShader($slangpPath)` / `setShader(null)`
(librashader; Vulkan on Android, Metal on iOS; ship presets yourself —
`Shaders::in($dir)` lists them), `screenshot(): ?string` (PNG path).

**Option units rule:** anything proportional is a whole percentage where 100
means unchanged (`gamma` 50–200, `volume` 0–100, `balance` −100..100).
Out-of-range is **rejected**, never clamped. Exceptions carry their unit in
the name (`speed` multiplier, `runAhead` frames, `rewindBufferSeconds`).

### Cheats

`addCheat('7E0010:01+7E0011:FF', $description)` — ares raw `ADDR:VALUE` hex
pairs joined by `+`; overrides CPU reads while active. Game Genie/GameShark
codes are not parsed (convert first). `removeCheat`, `clearCheats`,
`loadCheatsFile($path)` (desktop-ares `.cheats.bml`).

### Introspection

`status(): Status`, `accuracy(): ?Accuracy` (what the running core actually
bound), `region(): string`, `ports(): array`, `engineOptions(): array`,
`Emulator::systems()` (id, name, stable, supported, backends per system, and
`capabilities` — one object per backend with videoSettings/rumble/serialize/
cheats/memoryAccess/slottedMedia/multitap/mouse flags, `toggles` = the boolean
setSystemOptions keys that engine+system accepts, `bootOptions` = boot-only
booleans like pixelAccuracy/rawAudio). `Emulator::capabilities($system,
$backend)` returns one pair's object directly. Consumers should read
capabilities instead of hardcoding engine tables — the lists come from what
each core actually declares.

### Configs

`Config` (shareable house style) and per-system `SystemConfig` subclasses:
`SfcConfig`, `FcConfig`, `GbConfig` (gb+gbc), `GbaConfig`, `MdConfig`.

Base `Config` also carries two all-or-nothing booleans, both default **off**:

- `bootAnimation` — console boot intros (GB/GBC logo + ding, GBA BIOS) are
  **skipped by default**: GB/GBC boot animation-free boot ROMs with identical
  post-boot state, mGBA uses its native `skipBios`, and a hidden fast-forward
  covers the rest. `true` plays the real intros.
- `rawAudio` — the plugin's audio corrections (GB DC handling in both engines,
  the 60 Hz filter) are **on by default**; `true` restores each engine's own
  untouched output. Loudness matching and transition fades are calibration,
  not corrections, and always stay on.

All `Config` keys plus:

- `biosPath` — optional real-BIOS override (GBA only in practice).
- `backend` — `Backend` enum or string engine/core name (see Engines).
- `engineOptions` — `array<string,string>` for libretro cores (see below).
- `region` / `preferredRegions` (regional systems), `accuracy` /
  `pixelAccuracy` (SNES/GBA renderer, **boot-only** — post-boot configure
  throws `BOOT_ONLY_OPTION`), `deepBlackBoost` (SNES), `colorEmulation` +
  `interframeBlending` (GB).

### Events

`EmulatorStarted` (first frame — safe to query), `EmulatorStopped`,
`EmulatorPaused`, `EmulatorResumed`, `MemoryRead`, `MemoryChanged`,
`EmulatorError` (operational; carries `EmulatorErrorCode`). Listen with
NativePHP's `#[On(EmulatorStarted::class)]`.

### Elements

```blade
<native:emulator name="main" system="sfc" :config="$config"
    :system-config="$sfcConfig" :rom="$romPath"
    z-index="0" input-capture="global" />
```

Boot-only sugar: it stages/boots on mount and tears down on unmount; all
interaction goes through `Emulator::surface()`. Props are scalar-only —
configs cross as JSON internally. When booting imperatively instead, issue
the first bridge call after the page renders (first `#[Poll]` tick), never in
`mount()`.

```blade
<native:dpad surface="main" class="w-36 h-36"
    threshold="33" diagonal-ratio="0" thickness="36" radius="28"
    diagonals="true" color="#66FFFFFF" active-color="#E6FFFFFF" />
```

Finger position resolves natively (diagonals, slide-off-and-keep-walking; no
PHP per press). `@change` reports held directions (`"Up,Right"`, `""`).
`:pan-x`/`:pan-y` integrate into `SharedValue`s on the native frame clock
(`pan-speed`, per-axis min/max) for driving non-emulator UI without PHP.

## Engines and cores

Resolution order for who serves a boot:

1. **Explicit `backend:` on the system config — strict.** What you name
   must serve or `loadSystem` throws `UNSUPPORTED_BACKEND` listing what
   does. Never a silent substitution.
2. **The app map in `config/retro-emulator.php` — the recommended load
   order, one shape everywhere: `[fast pick, accurate pick]`**
   (`'sfc' => ['snes9x', 'bsnes']`). Entries are tried in order; an
   engine that isn't bundled/fetched is skipped **with a warning** (the
   Laravel log names the miss + the fetch-core command; the bridge logs
   it device-side too; copy-assets warns at build where hook output is
   surfaced) and ares is the implicit final fallback — the app always
   boots. The map is prescriptive: the plugin can't ship the fetchable
   cores itself (their licences), so the shipped config directs the dev
   to the recommended setup, and fetching a core activates it with no
   config edit. A plain string wraps to a one-item list.
3. **The built-in engine (ares)** — the floor when the list runs dry or no
   map entry exists.

Engines only ever run because a config named them — the plugin hardcodes no
pick. `engineOptions` in the same config implies a specific core; if the
preference list falls past that core, the option validation fails loudly
rather than applying to the wrong engine.

### Bring-your-own libretro cores

Drop `<core>_libretro_android.so` into
`resources/emulator-cores/android/<abi>/` (or run
`php artisan retro-emulator:fetch-core <names…>`, which downloads both ABIs
from the buildbot and prints each core's licence). `copy-assets` packages the
dir into the app with the `lib` prefix Android needs. Name the core as
`backend:` — bare name or full path. The loader validates
`retro_api_version == 1`, and:

- ROMs load from memory AND a staged file path under a core-declared
  extension (some cores, e.g. Mesen, identify format by extension;
  `need_fullpath` cores read the file).
- Battery saves ride `RETRO_MEMORY_SAVE_RAM` under the same `save.ram` name
  as every engine; states/rewind/run-ahead via `retro_serialize`.
- Cheats apply as per-frame RAM patches; memory access uses the catalog
  window; DRC skews the resampler.
- RetroPad ids equal the plugin's positional button bits — input just works.
- Adopting a new core while a game runs stages it; the swap lands at the
  next boot.
- iOS: no drop-in dir (signed-framework requirement, no buildbot slices);
  the loader probes `<name>_libretro_ios.dylib` — embed a self-built
  framework.

Hardware-verified cores (instrumented suite boots each on device; CPU = % of
one Snapdragon 8 Gen 2 core, same scenes as the ares numbers):

| Core | System | CPU | ares same scene |
|---|---|---|---|
| fceumm | fc | 6.9% | 28.6% |
| mesen | fc | 15.0% | 28.6% |
| snes9x | sfc | 8.1% | 20.6% |
| bsnes | sfc | 18.5% | 20.6% |
| picodrive | md | 7.0% | 45.0% |
| genesis_plus_gx | md | 8.6% | 45.0% |

Bundled for comparison: SameBoy 8.6% vs ares 27.7% (gb); mGBA 8.1% vs ares
51.7% (gba). Other cores load but are untested; software renderers only (no
hardware-GL cores). Every core's licence and what it obligates lives in
LICENSING.md — the app author's responsibility when shipping. A daily
GitHub Action (`core-canary.yml`) watches the buildbot URLs for drift.

### engineOptions

Libretro cores declare their own settings; `engineOptions` passes them
through with validation against that declared schema. Unknown key or
undeclared value → `UNSUPPORTED_OPTION` echoing the legal set. Bundled
engines declare none (their settings are the typed config keys) and refuse.
What a legal option does is the core author's contract — use at your own
risk. `engineOptions()` returns `[{key, choices, default, current}]`.

## Firmware / BIOS

Nothing required from the user: SFC IPL+boards, GB/GBC boot ROMs, MD TMSS
(all ares), and an open GBA BIOS (Cult-of-GBA, MIT) are embedded. GBA
optionally takes a real dump via `biosPath` (overrides the embedded one).

## Artisan commands

- `retro-emulator:fetch-core {cores*} {--abi=*}` — download cores into the
  drop-in dir; prints licence + untested note per core.
- `nativephp:retro-emulator:copy-assets` — build hook; ships prebuilt libs
  filtered by `config('retro-emulator.systems')` + any dropped-in cores.

`config/retro-emulator.php`: `systems` (null = all), `shaders` (bool),
`backends` (system → engine/core map).

## Invariants for contributors

- **Loud, not silent**: unsupported engines/options/values error with the
  legal alternatives. Never add a silent no-op or a silent fallback.
- **Percentage rule** for any proportional option; out-of-range rejects.
- **Positional button bits** are a cross-layer contract (Kotlin, Swift, PHP
  enums, catalog, RetroPad); Pest drift scanners parse the native sources —
  keep `system_catalog.cpp` literals and error-code strings scanner-friendly.
- **Rebuild prebuilts** after native changes (both scripts) and commit the
  `.so`s; iOS static cores need their link anchor added to the sum in
  `ios/core_link.cpp` (a bare read is elided at -O2 and the archive drops
  the object).
- Never commit: ROMs, BIOS dumps, downloaded cores (`vendor-engines/`,
  `resources/emulator-cores/`, `local/`), or anything under `.claude/`.
