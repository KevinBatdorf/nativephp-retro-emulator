# Retro Emulator for NativePHP Mobile

Full retro consoles inside your Laravel mobile app, on Android and iOS.
Native rendering, save states, rewind, cheats, shaders, real controllers.

```bash
composer require kevinbatdorf/retro-emulator
php artisan native:plugin:register kevinbatdorf/retro-emulator
```

> Requires NativePHP Mobile v4 (EDGE): `composer require nativephp/mobile:^4.0`.

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

## Show me

Step one: drop the element into a view. It boots when it mounts, and the
d-pad gives you optional on-screen touch controls:

```blade
@use('KevinBatdorf\RetroEmulator\Config\SfcConfig')

<native:emulator name="main" system="sfc" :rom="$romPath"
    :system-config="new SfcConfig(backend: 'snes9x')" />

<native:dpad surface="main" class="w-36 h-36" />
```

From PHP, `Emulator::surface('main')` binds that element and mutates the
running game. Everything is one call away:

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

Full API, events, elements, and engine internals: [AGENTS.md](AGENTS.md).

AI was used to build this plugin and the readme
