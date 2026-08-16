## kevinbatdorf/retro-emulator

Native retro-game emulation for NativePHP Mobile with pluggable engines — ares, SameBoy and
mGBA are bundled, and any fetched libretro core loads by name (Android only; iOS runs the
bundled engines). The emulator renders on a
native surface positioned by the EDGE layout engine; PHP drives the lifecycle and reads
emulated RAM live. There is **no JavaScript API** — all control is PHP-side.

### Installation

```bash
composer require kevinbatdorf/retro-emulator
php artisan native:plugin:register kevinbatdorf/retro-emulator
php artisan native:install
```

### Declaring the surface

The emulator is an EDGE element and must exist in the rendered layout before any bridge call.
The element can also boot declaratively — no imperative PHP needed to get a game on screen:

@verbatim
<code-snippet name="Emulator surface in an EDGE view" lang="blade">
<native:column class="flex-1">
    {{-- Surface only; boot imperatively later --}}
    <native:emulator name="main" class="flex-1" />

    {{-- Or boot-on-mount: system + config + rom in the element --}}
    <native:emulator name="main" class="flex-1"
        system="sfc" :config="$config" :rom="$romPath" />
</native:column>
</code-snippet>
@endverbatim

### PHP Usage

Entry point: `Emulator::surface('main')` returns the fluent handle for one surface; commands
return `$this`, queries return values. **Boot after first render, not in `mount()`** — the
native surface is created when the screen first renders, so bridge calls from `mount()`
cannot find it. Boot from a `#[Poll]` tick or any post-render interaction.

@verbatim
<code-snippet name="Boot and drive the emulator" lang="php">
use KevinBatdorf\RetroEmulator\Facades\Emulator;
use KevinBatdorf\RetroEmulator\Config\SfcConfig;
use KevinBatdorf\RetroEmulator\{Accuracy, Device, System};

$emu = Emulator::surface('main')
    ->loadSystem(System::Sfc, new SfcConfig(volume: 80, rewind: true))
    ->loadRom(storage_path('app/roms/game.sfc'), savePath: storage_path('app/saves'));

// Controllers are explicit — nothing is plugged in until connected.
$pad = $emu->connectDevice(1, Device::Gamepad);
$pad->press('A')->release('A');           // or the per-system button enums

$emu->pause()->resume();
$emu->saveState(slot: 1)->loadState(slot: 1)->undoSaveState();
$emu->setSpeed(2.0)->fastForward(true);

$bytes = $emu->readMemory(0x7E0010);       // sync WRAM read, returns int[]
$emu->watchMemory(0x7E0010, length: 2);    // fires MemoryChanged on change
$emu->setVideo(luminance: 90)->setVolume(80)->setShader($presetPath);

Emulator::systems();          // [{id, name, stable, supported, backends, capabilities}, …]
Emulator::capabilities('gb', 'sameboy');   // one engine's flags + toggles for a system
</code-snippet>
@endverbatim

Key API facts an assistant gets wrong without being told:

- State methods are `saveState`/`loadState`/`undoSaveState`/`undoLoadState` (slot int or name).
- `watchMemory(int $address, int $length = 1)` — one address per call, not an array.
- Multitaps use `connectMultitap(port, Device::SuperMultitap)` and return `Controller[]`;
  `connectDevice` returns one `Controller`.
- Typed configs (`SfcConfig`, `GbConfig`, `GbaConfig`, `FcConfig`, `MdConfig`) are preferred
  over arrays; every proportional option is a whole percentage where 100 = unchanged, and
  out-of-range values are **rejected**, not clamped. `speed` is the exception: a 0.25–4.0
  multiplier.
- `accuracy: Accuracy::Accurate` (or `pixelAccuracy: true`) is **boot-time only** — it picks
  the renderer at `loadSystem`; a post-boot `configure(['pixelAccuracy' => …])` throws
  `BOOT_ONLY_OPTION`. Readback via `$emu->accuracy()`; null on cores with one renderer
  (only SNES and GBA have two).
- No system needs a BIOS file — firmware is embedded (GBA boots an open BIOS; a real dump via
  `biosPath` is an optional accuracy override).
- `backend` picks the engine per boot (`'sameboy'`, `'mgba'`, or a libretro core name);
  omitted, each system boots its default engine. Fetched libretro cores are
  **Android-only** — iOS has the bundled engines (ares everywhere, SameBoy on gb/gbc,
  mGBA on gba), and a config preferring a fetched core falls back to ares there.
- Console boot intros are skipped by default — `bootAnimation: true` plays them. The plugin's
  audio corrections are also on by default — `rawAudio: true` restores the engine's own
  untouched output.

Supported systems in the current build: `fc`, `sfc`, `gb`, `gbc`, `gba`, `md`
(`Emulator::systems()` reports `supported` per system). Memory addresses are console bus
addresses; each system exposes its work-RAM window (sfc `0x7E0000`–`0x7FFFFF`,
fc `0x0000`–`0x07FF`, gb/gbc `0xC000`–`0xDFFF`, gba `0x02000000`–`0x0203FFFF`,
md `0xFF0000`–`0xFFFFFF`).

### On-screen d-pad

`<native:dpad surface="main" class="w-36 h-36" />` renders a touch d-pad whose presses go
straight into the core natively — no PHP per press. Feel and look are props (`threshold`,
`diagonal-ratio`, `thickness`, `radius`, `diagonals`, `color`, `active-color` — percentages).
It can also steer UI: `@change="method"` reports held directions ("Up,Right"), and
`:pan-x`/`:pan-y` integrate motion into `SharedValue`s on the native frame clock — never
animate via fast `#[Poll]` ticks, which destabilise the host.

### Events

All under `KevinBatdorf\RetroEmulator\Events`: `EmulatorStarted` (first rendered frame after
`loadRom()`), `EmulatorStopped`, `EmulatorPaused`, `EmulatorResumed`, `MemoryRead` (response
to `readMemoryAsync()`), `MemoryChanged` (watched address changed), `EmulatorError`
(operational failures — missing ROM, failed save — carrying an `EmulatorErrorCode`;
programmer errors throw `EmulatorException` synchronously instead), `WindowMetricsChanged`
(rotation/resize while an emulator surface is mounted — width/height/top/bottom/left/right
in dp, same shape as the `Emulator::windowMetrics()` query; use it to re-inset overlay
controls around the Dynamic Island in landscape, which EDGE's top/bottom-only safe-area
classes can't cover).

@verbatim
<code-snippet name="Listening for emulator events" lang="php">
use KevinBatdorf\RetroEmulator\Events\MemoryChanged;
use Native\Mobile\Attributes\On;

#[On(MemoryChanged::class)]
public function onMemoryChanged(string $surface, int $address, int $oldValue, int $newValue)
{
    // react to the RAM change
}
</code-snippet>
@endverbatim

### Platform notes

Everything the API exposes works on Android and iOS, with one asymmetry: bring-your-own
libretro cores load on Android only. `setShader` applies librashader
`.slangp` presets (a preset that fails to load reports an `EmulatorError`, `SHADER_FAILED`);
`setInputMapping` merges a per-port controller remap (`['a' => 'b', 'b' => 'a']` swaps A and
B; unknown buttons throw). Slotted media (SuFami Turbo, BS-X) load as
`loadRom(['base' => $biosRom, 'slotA' => $gameRom])`.
