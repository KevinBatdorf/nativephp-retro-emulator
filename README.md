# Retro Emulator for NativePHP Mobile

Native retro-game emulation for [NativePHP Mobile](https://nativephp.com) apps, powered by the
[ares](https://ares-emu.net) multi-system emulator core (ISC). The emulator renders on a real
native surface — `GLSurfaceView` on Android, `MTKView` on iOS — positioned by the v4 SuperNative
(EDGE) layout engine like any other native element. PHP drives the whole lifecycle and can read
and watch emulated RAM live.

> **Requires NativePHP Mobile v4 (SuperNative)** — the `dev-element` branch. The plugin registers
> a custom native element through v4's EDGE component registry, which does not exist on v3.

## Installation

```bash
composer require kevinbatdorf/retro-emulator
php artisan vendor:publish --tag=nativephp-plugins-provider
php artisan native:plugin:register kevinbatdorf/retro-emulator
php artisan native:install
```

Registration is not optional: NativePHP only compiles plugins listed in
`App\Providers\NativeServiceProvider::plugins()` into native builds —
`native:plugin:register` adds the entry for you.

Prebuilt native libraries ship with the plugin (hosts never compile the ares C++). To refresh
them after changing native code: `scripts/build_android_libs.sh` and `scripts/build_xcframework.sh`.

## Supported systems

Compiled into the current build (`GetSystems` reports `supported: true`):

| System | id | Firmware |
|---|---|---|
| NES / Famicom | `fc` | embedded |
| SNES / Super Famicom | `sfc` | embedded (ipl.rom + boards.bml) |
| Game Boy | `gb` | embedded (DMG boot ROM) |
| Sega Mega Drive / Genesis | `md` | embedded (TMSS) |

No BIOS files are required for these systems. `Emulator::getSystems()` lists every ares system
with `supported`, `stable`, and `biosRequired` flags; unsupported ids are rejected by
`loadSystem()` with `UNSUPPORTED_SYSTEM`. Adding a system means adding its core to both native
builds and one entry to `native/system_registry.cpp`.

## Usage

Declare the surface in an EDGE view:

```blade
<native:column class="flex-1">
    <native:emulator name="main" class="flex-1" />
</native:column>
```

Drive it from PHP:

```php
use KevinBatdorf\RetroEmulator\Facades\Emulator;

Emulator::boot('main')
    ->loadSystem('sfc', config: ['autoSave' => true])
    ->loadRom(storage_path('app/roms/game.sfc'));

// Live memory access (bus addresses; window is system-specific — see below)
$bytes = Emulator::readMemory(0x7E0010);      // sync, returns array of ints
Emulator::watchMemory([0x7E0010]);            // MemoryChanged event on change
```

> **Boot after first render, not in `mount()`.** The native surface is created when the screen
> first renders, which happens *after* `mount()` returns — bridge calls made in `mount()` cannot
> find the surface. Boot from a `#[Poll]` tick or any post-render interaction instead.

> **iOS is currently single-surface.** The iOS renderer registers under the name `main`
> regardless of the `name` attribute, so multiple simultaneous surfaces and custom surface
> names only work on Android for now. This lifts when the iOS EDGE renderer integration lands.

### Memory windows

`readMemory` / `writeMemory` / `watchMemory` operate on each console's work-RAM bus window:

| System | Window |
|---|---|
| `sfc` | `0x7E0000`–`0x7FFFFF` (128 KB WRAM) |
| `fc` | `0x0000`–`0x07FF` (2 KB RAM) |
| `gb` | `0xC000`–`0xDFFF` (8 KB WRAM) |
| `md` | `0xFF0000`–`0xFFFFFF` (64 KB work RAM) |

### Battery saves & save states

Battery-backed memory (`save.ram`, `save.eeprom`, …) persists automatically: it is seeded from
disk before the cartridge connects and flushed on pause, stop, teardown, and every 30 s while
running (`config: ['autoSave' => false]` disables the interval). Files are keyed by surface and
ROM basename under app storage. Save states are separate full-machine snapshots:
`stateSave(slot:)` / `stateLoad(slot:)` / `undoStateSave()` / `undoStateLoad()`.

## Feature support (v1)

Implemented: pause/resume/stop, save states, battery saves, sync + async memory reads, memory
watches, software input (`pressButton`/`setButtons`), hardware controllers, screenshots,
`fastForward`, `configure(['speed' => 0.25–4.0])`, `setAudio(volume/balance)`,
`setVideo(luminance/saturation/gamma/colorBleed/interframeBlending)`.

Not implemented in v1 — these return a `NOT_IMPLEMENTED` bridge error instead of silently
succeeding: `runAhead`, `rewind`, `setShader` (librashader), cheats, rumble, custom input
mappings. `setVideo` options ares has no post-processing hook for (`colorEmulation`,
`deepBlackBoost`, `overscan`, `pixelAccuracy`) are accepted and reported back as `ignored`.

## Events

| Event | Payload | When |
|---|---|---|
| `EmulatorStarted` | `system`, `romPath` | first rendered frame after `loadRom()` |
| `EmulatorStopped` / `EmulatorPaused` / `EmulatorResumed` | — | lifecycle |
| `MemoryRead` | `address`, `bytes` | response to `readMemoryAsync()` |
| `MemoryChanged` | `address`, `oldValue`, `newValue` | watched address changed |
| `EmulatorError` | `code`, `message` | runtime failures |

All under `KevinBatdorf\RetroEmulator\Events`. Call-site failures come back inline as bridge
errors, not events.

## Bridge functions

All 35 functions are declared in `nativephp.json` under the `Emulator.*` namespace — Boot,
LoadSystem, LoadRom, Pause, Resume, Stop, StateSave/Load, UndoStateSave/Load,
ReadMemory(Async), WriteMemory, Watch/Unwatch/ClearMemoryWatches, SetAudio, SetVideo,
Configure, SetSystemOptions, FastForward, SetInputMapping, SetRumble, SetShader,
Add/Remove/ClearCheats, Press/ReleaseButton, SetButtons, Screenshot, GetStatus, GetRegion,
GetPorts, GetSystems. The PHP `Emulator` class (and facade) wraps every one.

## ROMs

The plugin ships no game content and never should. Homebrew test ROMs for the plugin's own test
suites are fetched by `scripts/fetch_test_roms.sh` into the gitignored `tests/roms/`.

## License

MIT. The bundled ares emulator core is ISC — see `ares/LICENSE`.
