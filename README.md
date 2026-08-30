# Retro Emulator for NativePHP Mobile

Native emulation for six consoles, NES through Genesis — zero setup.

## Install

```bash
composer require kevinbatdorf/retro-emulator
php artisan native:plugin:register kevinbatdorf/retro-emulator
```

Requires NativePHP Mobile v4. No BIOS files, no API keys, no setup — your
first build for each platform downloads the prebuilt emulator cores
automatically (checksum-verified, cached from then on).

The bundled engines (ares, SameBoy, mGBA — all permissively licensed) play
everything. Want a lighter or more accurate core? Fetch it once and your
builds bundle it from then on:

```bash
php artisan retro-emulator:fetch-core snes9x
```

| System | Fast | Accurate |
|---|---|---|
| NES | fceumm † | mesen † |
| SNES | snes9x † | bsnes † |
| Game Boy / Color | SameBoy | ares |
| GBA | mGBA | ares |
| Genesis | picodrive † | genesis_plus_gx † |

† fetched, not bundled — GPL and non-commercial licences stay out of your
app unless you opt in; read [LICENSING.md](LICENSING.md) before shipping
one. Fetched cores are Android-only; iOS plays everything through the
bundled engines. Any other libretro core loads the same way.

## A console in two lines

```blade
<native:emulator name="main" system="sfc" :rom="$romPath" />

<native:dpad surface="main" class="w-36 h-36" />
```

The element boots when it mounts. The d-pad is optional on-screen touch
controls — physical gamepads work with zero setup, and port 1 connects the
system's default pad automatically.

## Play with the running game

```php
use KevinBatdorf\RetroEmulator\{Emulator, Device};
use KevinBatdorf\RetroEmulator\Buttons\SfcButton;

$emu = Emulator::surface('main');

$emu->saveState();                // instant snapshot, with undo
$emu->toggleRewind();             // play time backwards
$emu->setSpeed(2.0);              // or slow-mo at 0.5
$emu->setShader($crtPreset);      // real CRT shaders (librashader)
$emu->addCheat('7E0010:01');      // live RAM patches
$emu->connectDevice(1, Device::Gamepad)->press(SfcButton::A);
$emu->watchMemory(0x7EF340, 2);   // react as the game writes RAM
```

That memory watch means your Livewire component updates the moment Link
picks up a heart piece:

```php
#[On(MemoryChanged::class)]
public function onGameProgress(array $payload) { /* … */ }
```

Inertia + Vue/React apps skip PHP entirely — every native function has a
named JavaScript export:

```js
import Emulator, { LoadRom, StateSave, onNativeEvent }
    from './vendor/kevinbatdorf/retro-emulator/resources/js/index.js';

await Emulator.Boot({ system: 'snes' });
await LoadRom({ path: romPath });
onNativeEvent('EmulatorStarted', () => console.log('first frame'));
```

## What's in the box

- **Six systems**, playable out of the box, verified on real hardware.
- **Save states** with undo, battery saves, instant **rewind**,
  fast-forward, run-ahead.
- **Controllers**: touch d-pad element, physical gamepads, 4-player
  multitap, mouse, rumble.
- **Live memory**: read, write, and watch RAM — build trackers, trainers,
  and game-reactive UI.
- **Presentation**: CRT shaders, color and gamma controls, aspect
  correction, screenshots.
- **Cheats**, region control, accuracy toggles, an OS ROM picker
  (`pickRom()`), and window/safe-area metrics for overlay layouts.

## Events

`EmulatorStarted`, `EmulatorStopped`, `EmulatorPaused`, `EmulatorResumed`,
`MemoryRead`, `MemoryChanged`, `EmulatorError`, `RomPicked`, and
`WindowMetricsChanged` — listen with `#[On(...)]` in Livewire or
`onNativeEvent` in JS. All under `KevinBatdorf\RetroEmulator\Events\`.

## Permissions and secrets

Android asks for `VIBRATE` (controller rumble); iOS asks for nothing. The
shipped app makes no network, storage, camera, or location access, and the
plugin needs no API keys or environment variables.

## The deep end

The full reference — every method and config key, element props, engine
resolution rules, bring-your-own libretro cores, memory windows, build
internals — lives in [AGENTS.md](AGENTS.md). Licence obligations live in
[LICENSING.md](LICENSING.md) and
[THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).

<details>
<summary>AI Disclosure</summary>

This project was built by the developer using AI tooling and autonomous
coding agents. Design, architecture, and product decisions are human;
implementation was AI-assisted under direction, with every change reviewed
and verified on real hardware before shipping.

However, AI wrote the above too (but the dev wrote this line!), so use your own judgement.

</details>
