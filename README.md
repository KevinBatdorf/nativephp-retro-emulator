# Retro Emulator for NativePHP Mobile

Full retro consoles inside your Laravel mobile app. Native rendering, save
states, rewind, cheats, shaders, real controllers — Android and iOS.

```bash
composer require kevinbatdorf/retro-emulator
php artisan native:plugin:register kevinbatdorf/retro-emulator
```

> Tracks NativePHP Mobile v4 (EDGE). Until v4 tags, point composer at the
> `dev-element` branch.

## Engines

Every system plays out of the box — no BIOS files, nothing to download.
Prefer a specific engine? Our picks:

| System | Fast | Accurate |
|---|---|---|
| NES | fceumm (GPL-2.0+) | mesen (GPL-3.0) |
| SNES | snes9x (non-commercial) | bsnes (GPL-3.0) |
| Game Boy / Color | SameBoy (MIT) † | ares (ISC) † |
| GBA | mGBA (MPL-2.0) † | ares (ISC) † |
| Genesis | picodrive (non-commercial) | genesis_plus_gx (non-commercial) |

† ships in the box. Everything else is one download away — then name it per
boot, or app-wide in `config/retro-emulator.php`:

```bash
php artisan retro-emulator:fetch-core snes9x
```

We test the cores above on real hardware; any other libretro core loads the
same way. Each core carries its own licence — see
[LICENSING.md](LICENSING.md). On iOS extra cores are embedded as self-built
frameworks instead of downloaded (see [AGENTS.md](AGENTS.md)).

## Show me

Step one: drop the element into a view — it boots when it mounts. The d-pad
is optional on-screen touch controls:

```blade
@use('KevinBatdorf\RetroEmulator\Config\SfcConfig')

<native:emulator name="main" system="sfc" :rom="$romPath"
    :system-config="new SfcConfig(backend: 'snes9x')" />

<native:dpad surface="main" class="w-36 h-36" />
```

From PHP, `Emulator::surface('main')` binds that element and mutates the
running game — everything is one call away:

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

Prefer to boot from PHP? Leave the setup attributes off the element and load
imperatively — including a core's own settings, validated against what it
declares:

```php
$emu->loadSystem(System::Sfc, new SfcConfig(
    backend: 'snes9x',
    engineOptions: ['snes9x_region' => 'pal'],
))->loadRom($romPath);
```

Full API, events, elements, and engine internals: [AGENTS.md](AGENTS.md).
