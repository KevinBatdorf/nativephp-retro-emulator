# Retro Emulator for NativePHP Mobile

Full retro consoles inside your NativePHP mobile app, on Android and iOS.
Native rendering, save states, rewind, cheats, shaders, real controllers.

## Installation

```bash
composer require kevinbatdorf/retro-emulator
php artisan native:plugin:register kevinbatdorf/retro-emulator
```

> Requires NativePHP Mobile v4 (EDGE): `composer require nativephp/mobile:^4.0`.

No API keys, secrets, or environment variables are required — the plugin
works entirely on-device.

## Engines

Every system plays out of the box. No BIOS files, nothing to download.
Prefer a specific engine? Our picks:

| System | Fast | Accurate |
|---|---|---|
| NES | fceumm † (GPL-2.0+) | mesen † (GPL-3.0) |
| SNES | snes9x † (non-commercial) | bsnes † (GPL-3.0) |
| Game Boy / Color | SameBoy (MIT) | ares (ISC) |
| GBA | mGBA (MPL-2.0) | ares (ISC) |
| Genesis | picodrive † (non-commercial) | genesis_plus_gx † (non-commercial) |

† requires a download. The plugin bundles only permissively licensed
engines, so GPL and non-commercial cores stay out of your app unless you
opt in. Fetching one is a single command, run once on your machine, and
your builds bundle it from then on:

```bash
php artisan retro-emulator:fetch-core snes9x
```

Pick the engine per boot, or app-wide in `config/retro-emulator.php`. We
test every core in the table on real hardware, and any other libretro core
loads the same way. Read [LICENSING.md](LICENSING.md) before shipping a
fetched core.

**Fetched cores are Android-only.** libretro publishes no usable iOS
builds, so iOS ships the bundled engines (ares, SameBoy, mGBA). A config
that prefers a fetched core still works everywhere — engines that are not
present are skipped and the boot lands on the built-in engine, ares.
Embedding a core you compiled yourself as a signed framework is possible
but on you; the loader probes `<name>_libretro_ios.dylib`.

| System | Android | iOS |
|---|---|---|
| NES | ares, fceumm, mesen | ares |
| SNES | ares, snes9x, bsnes | ares |
| Game Boy / Color | ares, SameBoy | ares, SameBoy |
| GBA | ares, mGBA | ares, mGBA |
| Genesis | ares, picodrive, genesis_plus_gx | ares |

Android can bundle any additional libretro core the same way — fetch it
(or drop the `.so` into `resources/emulator-cores/`) and it appears in
the next build.

## Usage (PHP)

Step one: drop the element into a view. It boots when it mounts, and the
d-pad gives you optional on-screen touch controls:

```blade
@use('KevinBatdorf\RetroEmulator\Config\SfcConfig')

<native:emulator name="main" system="sfc" :rom="$romPath"
    :system-config="new SfcConfig(backend: 'snes9x')" />

<native:dpad surface="main" class="w-36 h-36" />
```

From PHP, `Emulator::surface('main')` binds that element and mutates the
running game. Commands are fluent; queries return values. Everything is
one call away:

```php
use KevinBatdorf\RetroEmulator\{Emulator, System, Device};
use KevinBatdorf\RetroEmulator\Buttons\SfcButton;
use KevinBatdorf\RetroEmulator\Config\SfcConfig;

$emu = Emulator::surface('main');
$emu->saveState()->toggleRewind()->setSpeed(2.0);
$emu->setShader($crtPreset);
$emu->addCheat('7E0010:01');
$emu->connectDevice(1, Device::Gamepad)->press(SfcButton::A);
$emu->watchMemory(0x7EF340, length: 2);   // fires events as the game writes
```

Prefer to boot from PHP? Leave the setup attributes off the element and
load imperatively. Engine options are validated against what the core
declares:

```php
$emu->loadSystem(System::Sfc, new SfcConfig(
    backend: 'snes9x',
    engineOptions: ['snes9x_region' => 'pal'],
))->loadRom($romPath);
```

Operational failures dispatch an `EmulatorError` event; programmer errors
throw `EmulatorException` synchronously.

### Boot and playback

| Call | Notes |
|---|---|
| `loadSystem(System\|string, SystemConfig\|array)` | Stages system + engine + options; synchronous validation |
| `loadRom(string\|array, ?string $savePath)` | Boots; call again to swap ROMs. Array form for slotted media: `['base' => $biosRom, 'slotA' => $cart, …]` |
| `pause()` / `resume()` / `stop()` | |
| `setSpeed(float)` | 0.25–4.0 multiplier, 1.0 native |
| `fastForward(bool)` | |
| `toggleRewind()` | Needs `rewind: true` in config; throws `REWIND_DISABLED` otherwise |
| `configure(array)` | Runtime merge: `speed`, `runAhead`, `rewind`, `rewindBufferSeconds`, `engineOptions` |
| `setSystemOptions(array)` | Per-system toggles; the legal set is `capabilities()['toggles']` |

### Save states and battery saves

`saveState($slot)` / `loadState($slot)` / `undoSaveState()` /
`undoLoadState()`. Battery saves persist under `loadRom(savePath:)`;
flushed on ROM swap and stop. States are engine-tagged — loading a state
saved by a different engine errors instead of corrupting.

### Memory

`readMemory($addr, $len)` (sync array of bytes), `readMemoryAsync()`
(→ `MemoryRead` event), `writeMemory($addr, bytes[])`,
`watchMemory($addr, $len)` (→ `MemoryChanged` on change),
`unwatchMemory()`, `clearMemoryWatches()`. Addresses are the system's
work-RAM bus window (e.g. SNES `0x7E0000+`).

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

Port 1 auto-connects the system's default pad at boot; `connectDevice`
is for everything else. Physical gamepads work with no setup.

### Presentation and audio

`setVideo(luminance:, saturation:, gamma:, colorBleed:, overscan:,
output:, fixedScale:, aspectCorrection:)` — merge-per-key.
`setVolume(0–100)`, `setBalance(−100–100)`,
`setShader($slangpPath)` / `setShader(null)` (librashader; Vulkan on
Android, Metal on iOS; ship presets yourself — `Shaders::in($dir)` lists
them), `screenshot(): ?string` (PNG path).

Anything proportional is a whole percentage where 100 means unchanged
(`gamma` 50–200, `volume` 0–100, `balance` −100..100). Out-of-range is
rejected, never clamped.

### Cheats

`addCheat('7E0010:01+7E0011:FF', $description)` — raw `ADDR:VALUE` hex
pairs joined by `+`. Game Genie/GameShark codes are not parsed (convert
first). `removeCheat()`, `clearCheats()`, `loadCheatsFile($path)`
(desktop-ares `.cheats.bml`).

### Introspection

`status()`, `accuracy()`, `region()`, `ports()`, `engineOptions()`,
`Emulator::systems()`, `Emulator::capabilities($system, $backend)` —
read capabilities instead of hardcoding engine tables; the lists come
from what each core actually declares. `Emulator::windowMetrics()`
returns window size and system-obscured insets (bars, Dynamic Island,
cutout) in dp, no surface required.

Full engine internals, configs, and element props: [AGENTS.md](AGENTS.md).

## Usage (JavaScript)

SPA frontends (Inertia + Vue/React) can call every bridge function
directly — `resources/js/index.js` exports one function per bridge
function, plus an `onNativeEvent` helper:

```js
import Emulator, { LoadRom, SaveState, onNativeEvent }
    from './vendor/kevinbatdorf/retro-emulator/resources/js/index.js';

await Emulator.Boot({ system: 'snes' });
await LoadRom({ path: romPath });
await SaveState({ slot: 1 });

const off = onNativeEvent('EmulatorStarted', (payload) => {
    console.log('first frame', payload);
});
off(); // unsubscribe
```

Params pass to the native layer verbatim — the same shapes the PHP API
sends. Under the hood every export POSTs to NativePHP's
`/_native/api/call` bridge endpoint.

Native events reach the page as a `native-event` CustomEvent on
`document`, so you can also listen without the helper:

```js
document.addEventListener('native-event', ({ detail }) => {
    // detail.event is the PHP event class, detail.payload the data
});
```

## Events

| Event | Fires |
|---|---|
| `EmulatorStarted` | First frame rendered — safe to query state |
| `EmulatorStopped` | Emulation ended |
| `EmulatorPaused` / `EmulatorResumed` | Playback state changes |
| `MemoryRead` | Async read completed (`readMemoryAsync`) |
| `MemoryChanged` | A watched address changed (`watchMemory`) |
| `EmulatorError` | Operational failure; carries `EmulatorErrorCode` |
| `RomPicked` | The OS file picker returned (`pickRom`) |
| `WindowMetricsChanged` | Rotation/resize while a surface is mounted |

All under `KevinBatdorf\RetroEmulator\Events\`. In Livewire, listen with
NativePHP's `#[On(EmulatorStarted::class)]`; in JS, use `onNativeEvent`
above.

## Permissions

| Platform | Permission | Used for |
|---|---|---|
| Android | `android.permission.VIBRATE` | Controller rumble (`setRumble`) |
| iOS | — none — | |

That's the whole list. No network, storage, camera, or location access.

## Secrets

None. The plugin needs no API keys, tokens, or environment variables.

<details>
<summary>AI Disclosure</summary>

This project was built by the developer using AI tooling and autonomous
coding agents. Design, architecture, and product decisions are human;
implementation was AI-assisted under direction, with every change reviewed
and verified on real hardware before shipping.

However, AI wrote the above too (but the dev wrote this line!), so use your own judgement.

</details>
