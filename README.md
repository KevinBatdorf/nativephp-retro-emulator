# Retro Emulator for NativePHP Mobile

Wraps the [ares](https://ares-emu.net) multi-system emulator as a NativePHP
Mobile plugin: prebuilt native binaries, a typed and fluent PHP API, and
in-layout rendering through a `<native:emulator>` element. Android and iOS.

## Installation

```bash
composer require kevinbatdorf/retro-emulator
```

Don't forget to register the plugin:

```bash
php artisan native:plugin:register kevinbatdorf/retro-emulator
```

> **Pre-release note:** the plugin tracks NativePHP Mobile v4 (EDGE), currently
> the `dev-element` branch — until v4 tags, `composer require` needs that
> branch resolvable (a VCS `repositories` entry, or a path repo to a checkout).

## Quick start — the declarative element

The element carries its own setup: native stages, boots, and applies the
config when the surface mounts, and tears down on unmount. No imperative PHP
is required to get a game on screen.

```blade
<native:emulator name="main" system="sfc"
    :config="$config"            {{-- global Config: cross-system house style --}}
    :system-config="$sfcConfig"  {{-- SfcConfig: overrides + sfc-only keys --}}
    :rom="$romPath" />
```

```php
use KevinBatdorf\RetroEmulator\Config\{Config, SfcConfig};
use KevinBatdorf\RetroEmulator\{InputCapture, Region};

$config = new Config(volume: 80, inputCapture: InputCapture::Global, rewind: true);
$sfcConfig = new SfcConfig(region: Region::Ntsc, deepBlackBoost: true);
```

## Runtime control — the surface handle

Commands are fluent (they return `$this`); queries return values.

```php
use KevinBatdorf\RetroEmulator\{Emulator, System, Device};
use KevinBatdorf\RetroEmulator\Buttons\SfcButton;

$emu = Emulator::surface('main');

// Boot imperatively instead of (or on top of) the declarative element.
$emu->loadSystem(System::Sfc, $sfcConfig)->loadRom($romPath, savePath: $dir);

// Playback.
$emu->pause()->resume()->stop();
$emu->setSpeed(2.0)->fastForward(true)->toggleRewind();

// Controllers are explicit — nothing is plugged in until you connect it.
$pad = $emu->connectDevice(1, Device::Gamepad);           // Controller handle
$pad->press(SfcButton::A)->release(SfcButton::A);

$mouse = $emu->connectDevice(1, Device::Mouse);
$mouse->setAxis('X', 5);                                   // relative delta

$scope = $emu->connectDevice(2, Device::SuperScope);
$scope->aimAt(0.5, 0.5)->press('Trigger');                 // normalized 0..1

$players = $emu->connectMultitap(2, Device::SuperMultitap); // Controller[4]

// Remap: the in-game button reads another positional input.
$pad->remap(['a' => 'b', 'b' => 'a']);

// States, saves, cheats, memory.
$emu->saveState(slot: 1)->loadState(slot: 1)->undoSaveState();
$emu->addCheat('7E0010:01', description: 'Example')->loadCheatsFile($path);
$emu->readMemory(0x7E0010);
$emu->writeMemory(0x7E0010, [0x01]);
$emu->watchMemory(0x7EF340, length: 2);   // fires MemoryChanged events

// Presentation + audio merge per key — omitted options keep their values.
$emu->setVideo(luminance: 100)->setVolume(80)->setBalance(0);
$emu->setShader($presetPath);
$emu->screenshot();

Emulator::systems();   // rich objects: id, name, stable, supported
```

### Option units

One rule covers nearly everything: **anything proportional is a whole percentage
where 100 means unchanged.**

| Option | Range | Meaning |
| --- | --- | --- |
| `luminance`, `saturation` | 0–100 | 100 leaves the picture untouched |
| `gamma` | 50–200 | 100 untouched; above darkens midtones |
| `volume` | 0–100 | per-emulator gain, not device volume |
| `balance` | −100–100 | full left … full right |
| `<native:dpad>` `threshold`, `diagonal-ratio`, `thickness`, `radius` | see below | percentages of the pad |

Out-of-range values are **rejected**, not clamped, so a wrong unit surfaces as an
error instead of a black screen or a dead control.

Two deliberate exceptions, both stated in their own docblocks: `speed` is a
0.25–4.0 **multiplier** (1.0 = native speed, too universal a convention to
bend), and counts carry their unit in the name — `runAhead` (frames),
`rewindBufferSeconds`, `fixedScale`.

### Accuracy vs performance

The plugin ships ares' **performance** renderers by default — per-scanline,
what phones comfortably afford. Where ares offers a more accurate renderer,
one boot-time switch turns it on:

```php
use KevinBatdorf\RetroEmulator\Accuracy;

new SfcConfig(accuracy: Accuracy::Accurate);   // the preset
new GbaConfig(pixelAccuracy: true);            // direct ares flag; beats the preset
```

| System | `performance` (default) | `accurate` |
|---|---|---|
| SNES | scanline PPU | dot-accurate PPU — mid-scanline raster effects render right |
| GBA | scanline, approximated bus timing | per-cycle rendering + real VRAM/palette contention |
| NES, GB/GBC, Mega Drive | one renderer — the option is accepted and ignored | same |

Rules worth knowing:

- **Boot-time only.** It picks the renderer implementation at `loadSystem`;
  changing it means rebooting. A post-boot `configure(['pixelAccuracy' => …])`
  throws (`BOOT_ONLY_OPTION`) rather than silently deferring — ares' own
  frontend also only honors the value at the next load.
- **Readback is honest:** `$emu->accuracy()` (and the `accuracy` key on
  status) reports which renderer the running core actually bound — `null` on
  systems with a single renderer.
- **The cost is real.** Measured on a Snapdragon 8 Gen 2 handheld at full
  60 fps: SNES 28% → 46% of one core, GBA 59% → 65%. Most games render
  pixel-identically either way; accuracy matters for mid-line tricks,
  contention-sensitive timing, and homebrew development against real
  hardware behavior.

### On-screen d-pad

`<native:dpad />` needs nothing but a surface; every prop has a default. Size
comes from the usual layout classes, the rest is styling and feel:

```blade
<native:dpad surface="play" class="w-36 h-36"
    threshold="33"        {{-- % of half-extent before a direction engages --}}
    diagonal-ratio="0"    {{-- % the weaker axis must reach; 0 = free diagonals --}}
    thickness="36"        {{-- arm width, % of the shorter side --}}
    radius="28"           {{-- corner rounding, % of arm width; 50 = round tip --}}
    diagonals="true"      {{-- false locks it to four ways --}}
    color="#66FFFFFF" active-color="#E6FFFFFF" />
```

Presses go straight into the core natively, so no PHP runs per press and nothing
re-renders.

To drive something other than the core, the pad reports its held directions
(`@change="method"` gives `"Up,Right"`, or `""` on release) — and for smooth
motion it can integrate them into `SharedValue`s on the native frame clock:

```blade
@php $x = SharedValue::make(0); $y = SharedValue::make(0); @endphp

<native:column class="w-8 h-8 rounded-full bg-red-500"
    :translate-x="$x" :translate-y="$y" />

<native:dpad class="w-28 h-28" :pan-x="$x" :pan-y="$y" pan-speed="260"
    pan-x-min="0" pan-x-max="760" pan-y-min="0" pan-y-max="320" />
```

Bound this way nothing moves through PHP, which is the point: a `#[Poll]` fast
enough to animate destabilises the host. Give each axis its own range — a screen
is not square, and an unbounded value integrates until whatever it moves is off
the edge.

> **Imperative boot caveat:** the surface must exist before the first bridge
> call resolves. When booting from PHP, do it after the page renders (for
> example on the first `#[Poll]` tick), not during `mount()` — the declarative
> element has no such constraint and is the taught default.

## Slotted media (SuFami Turbo, Satellaview)

Slot carts load as an array through the one boot path; the base cartridge is
the user-supplied BIOS ROM:

```php
$emu->loadRom(['base' => $sufamiBiosPath, 'slotA' => $gamePath]);
$emu->loadRom(['base' => $bsxBiosPath,    'slotA' => $cassettePath]);
```

## Systems

Source of truth at runtime is `Emulator::systems()` — `supported: true` means
the core is compiled into the shipped binaries, `stable: false` flags a core
we consider not yet production-ready. Per-system game compatibility:
`https://ares-emu.net/compatibility/<system>`.

| System | id | Compiled today | Default engine | Notes |
|---|---|---|---|---|
| NES / Famicom | `fc` | ✅ | ares | |
| SNES / Super Famicom | `sfc` | ✅ | ares | Full feature set, incl. peripherals + slotted media |
| Game Boy / Game Boy Color | `gb` / `gbc` | ✅ | SameBoy | ares selectable for its node-tree feature set |
| Game Boy Advance | `gba` | ✅ | mGBA | HLE BIOS built in; supply a real one via `biosPath` for accuracy |
| Mega Drive / Genesis | `md` | ✅ | ares | |

Other ares systems appear in `Emulator::systems()` with `supported: false` —
they aren't compiled into the shipped binaries.

### Engines

Phones want speed: where the plugin bundles a permissively-licensed fast
core, it is that system's default engine, and ares stays available as the
accuracy option. Measured on an AYN Thor (SD 8 Gen 2), same ROM and scene:
SameBoy runs Game Boy at 8.4% of one core where ares needs 27.8%, and mGBA
runs Game Boy Advance at 8.2% where ares needs 51.7%. `Emulator::systems()`
reports each system's `backends` and `defaultBackend`. Pick per boot with
the config —

```php
use KevinBatdorf\RetroEmulator\Backend;

Emulator::surface()->loadSystem(System::Gb, new GbConfig(backend: Backend::Ares));
```

— or app-wide in `config/retro-emulator.php` (`'backends' => ['gb' => 'ares']`).
An engine that doesn't serve the system throws `UNSUPPORTED_BACKEND`, and an
option the active engine lacks (e.g. `interframeBlending` on SameBoy) throws
`UNSUPPORTED_OPTION` — never a silent no-op.

### Bring your own libretro core

Any [libretro](https://www.libretro.com) core can serve a supported system —
the plugin bundles a loader (not an engine) that adopts a core when you name
it as the backend:

```php
Emulator::surface()->loadSystem(System::Sfc, new SfcConfig(backend: 'snes9x'));
```

#### Verified cores

These cores are exercised by this repo's own device test suite — each one
must adopt, boot a real ROM, render, and round-trip a save state on hardware
before it earns a row. One speed-focused and one accuracy-focused pick per
system:

| System | Speed pick | Accuracy pick |
|---|---|---|
| NES | FCEUmm — GPL-2.0 — 6.9% of one core | Mesen — GPL-3.0 — 15.0% |
| SNES | Snes9x — non-commercial only — 8.1% | bsnes — GPL-3.0 — 18.5% |
| Genesis | PicoDrive — MAME-style non-commercial — 7.0% | Genesis Plus GX — non-commercial — 8.6% |
| GB / GBC | *bundled:* SameBoy (Expat) — the community's speed **and** accuracy pick | |
| GBA | *bundled:* mGBA (MPL-2.0) — same story | |

CPU numbers are the same Thor measurement as the Engines section (ares on
the same scenes: NES 28.6%, SNES 20.6–29.5%, Genesis 45.0%). Genesis is the
starkest case — no bundled fast core, and either verified pick runs ~5–6×
lighter than ares.

**Every other libretro core is supported through the same loader but not
necessarily tested.** If it speaks libretro API v1 and renders in software,
it should work — cores demanding hardware GL contexts won't. You own the
testing (and the licence) for anything outside this table.

#### Adding a core

1. Fetch it — either command below lands the `.so` in
   `resources/emulator-cores/android/<abi>/`, where `copy-assets` packages
   it into your app and prints its licence line at build time:

```bash
php artisan retro-emulator:fetch-core mesen
```

   or grab it yourself from the
   [libretro buildbot](https://buildbot.libretro.com/nightly/android/latest/)
   and drop it in that directory — filenames ship as-is.

2. Name it per boot (`new FcConfig(backend: 'mesen')`) or app-wide in
   `config/retro-emulator.php` (`'backends' => ['fc' => 'mesen']`). The name
   is the file's core name; a full filesystem path also works. If no bundled
   engine or packaged core answers the name, `loadSystem` throws
   `UNSUPPORTED_BACKEND` naming what's available.

BYO cores pay off the same way the bundled ones do (same device, method, and
scenes as above): Snes9x runs SNES at 8.1% of one core where ares needs
20.6%, and FCEUmm runs NES at 6.9% where ares needs 28.6%.

#### Engine options

Libretro cores declare their own settings, and `engineOptions` passes them
through — validated, never blind:

```php
Emulator::surface()->loadSystem(System::Sfc, new SfcConfig(
    backend: 'snes9x',
    engineOptions: ['snes9x_region' => 'pal'],
));

Emulator::surface()->engineOptions();
// [['key' => 'snes9x_region', 'choices' => ['auto', 'ntsc', 'pal'],
//   'default' => 'auto', 'current' => 'pal'], …]

Emulator::surface()->configure(['engineOptions' => ['snes9x_region' => 'ntsc']]);
```

A key the core doesn't declare, or a value outside its declared choices,
throws `UNSUPPORTED_OPTION` echoing the legal set — a typo errors, it never
silently no-ops. What a declared option *does* is the core author's contract,
not this plugin's: use at your own risk. The bundled engines declare no
engine options (their settings are the typed config keys), so any
`engineOptions` on ares/SameBoy/mGBA throws `UNSUPPORTED_OPTION` too.

What a BYO core gets: the same host as bundled engines — save states, rewind,
run-ahead, battery saves, cheats (as RAM patches), memory read/write on the
catalog window, dynamic rate control, and the positional gamepad (RetroPad ids
match the plugin's button bits). Cores that read media by path
(`need_fullpath`, e.g. FCEUmm) work too — the ROM is staged to a file beside
the game's saves automatically. What a BYO core doesn't get: the wrapper's
per-engine options (`interframeBlending`, screen color settings, …) stay
`UNSUPPORTED_OPTION` unless the engine is bundled.

Licensing is yours to own when you ship a core. Two worked examples:
**Snes9x** is non-commercial-only — fine for a personal build, not for a paid
app; **fceumm** (and many others) are GPL, which obligates source availability
for your combined app when distributed. `copy-assets` prints one licence line
per core it bundles so nothing ships unnoticed. The bundled engines
(ares ISC, SameBoy Expat, mGBA MPL-2.0) keep the plugin itself
permissively licensed.

On iOS there is no drop-in dir: iOS requires dynamic libraries to be embedded,
signed frameworks, and the libretro buildbot doesn't publish iOS slices. The
loader is compiled in and probes `<name>_libretro_ios.dylib` — embed a
self-built core as a framework in your Xcode project and name it the same way.

## BIOS files

No system needs a BIOS from you — all firmware is embedded in the native
library: SFC `ipl.rom` + `boards.bml`, GB/GBC boot ROMs, MD TMSS (all from
ares), and an open GBA BIOS ([Cult-of-GBA](https://github.com/Cult-of-GBA/BIOS),
MIT) since ares ships none. Every system boots on `loadSystem` alone.

GBA optionally takes a real BIOS dump via `biosPath` on `GbaConfig` for
bit-accurate compatibility on edge-case titles — it overrides the embedded
one. Sourcing it is your responsibility (dump it from hardware you own); the
embedded BIOS covers the common case without it.

## Cheats

ares' raw format only: hex `ADDR:VALUE` pairs joined with `+`
(`7E0010:01+7E0011:FF`) — the value overrides every CPU read of the address
while active. See the [ares docs](https://ares-emu.net/docs) for details.

- **Game Genie / GameShark codes are not parsed** (not supported upstream in
  ares). Convert them to raw address:value pairs first — several web-based
  Game Genie decoders do this per system.
- `loadCheatsFile($path)` reads desktop ares' `.cheats.bml` list format, so
  lists are interchangeable with desktop.

## Shaders

`setShader($path)` applies a [librashader](https://github.com/SnowflakePowered/librashader)
`.slangp` preset per surface (Vulkan on Android, Metal on iOS);
`setShader(null)` clears it. `Shaders::in($dir)` lists presets for picker UIs.

Presets are not bundled — grab the
[libretro slang-shaders](https://github.com/libretro/slang-shaders) collection
and ship the presets you want with your app.

## Events

`EmulatorStarted`, `EmulatorStopped`, `EmulatorPaused`, `EmulatorResumed`,
`MemoryRead`, `MemoryChanged`, and `EmulatorError` (operational failures —
missing ROM, failed save, bad cheat — carry an `EmulatorErrorCode`; programmer
errors throw `EmulatorException` synchronously instead).

```php
use KevinBatdorf\RetroEmulator\Events\{EmulatorStarted, EmulatorError};
use Native\Mobile\Attributes\On;

#[On(EmulatorStarted::class)]
public function onStarted(string $surface = '', string $system = '', string $romPath = ''): void
{
    // First frame rendered — safe to read ports(), status(), region().
}

#[On(EmulatorError::class)]
public function onError(string $surface = '', string $code = '', string $message = ''): void
{
    // Operational failure (see EmulatorErrorCode) — the emulator is still in
    // a defined state; a failed loadRom leaves the previous game running.
}
```

## License

This plugin's own code is **MIT** (see [`LICENSE`](LICENSE)). It bundles the
ares emulator core and its dependencies, which are all permissive (ISC / MIT /
BSD / Apache-2.0) or file-level copyleft (librashader, MPL-2.0) — none reaches
your app's own license. Full attribution and per-component licenses are in
[`LICENSING.md`](LICENSING.md).
