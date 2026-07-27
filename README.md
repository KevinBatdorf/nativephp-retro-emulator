# Retro Emulator for NativePHP Mobile

Wraps the [ares](https://ares-emu.net) multi-system emulator as a NativePHP
Mobile plugin: prebuilt native binaries, a typed and fluent PHP API, and
in-layout rendering through a `<native:emulator>` element. Android and iOS.

```bash
composer require kevinbatdorf/retro-emulator
```

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

$players = $emu->connectDevice(2, Device::SuperMultitap);  // Controller[4]

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

| System | id | Compiled today | Notes |
|---|---|---|---|
| NES / Famicom | `fc` | ✅ | |
| SNES / Super Famicom | `sfc` | ✅ | Full feature set, incl. peripherals + slotted media |
| Game Boy / Game Boy Color | `gb` / `gbc` | ✅ | |
| Game Boy Advance | `gba` | ✅ | Boots on an embedded open BIOS; supply a real one via `biosPath` for accuracy |
| Mega Drive / Genesis | `md` | ✅ | |

Other ares systems appear in `Emulator::systems()` with `supported: false` —
they aren't compiled into the shipped binaries.

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

## License

This plugin's own code is **MIT** (see [`LICENSE`](LICENSE)). It bundles the
ares emulator core and its dependencies, which are all permissive (ISC / MIT /
BSD / Apache-2.0) or file-level copyleft (librashader, MPL-2.0) — none reaches
your app's own license. Full attribution and per-component licenses are in
[`LICENSING.md`](LICENSING.md).
