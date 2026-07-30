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
Want a different engine? Fetching one is a single command, and you pick it
per boot or app-wide.

```bash
php artisan retro-emulator:fetch-core snes9x
```

| System | In the box | One download away |
|---|---|---|
| NES | ares — accurate, heavier | fceumm — fast, light (GPL-2.0) · mesen — most accurate (GPL-3.0) |
| SNES | ares — accurate | snes9x — fast, runs everything (non-commercial) · bsnes — most accurate (GPL-3.0) |
| Game Boy / Color | ares · SameBoy — fast *and* accurate | |
| GBA | ares · mGBA — fast, battle-tested | |
| Genesis | ares — accurate, heaviest | picodrive — fastest (non-commercial) · genesis_plus_gx — accurate and still fast (non-commercial) |

We test every core in that table on real hardware. Any other libretro core
loads the same way — untested, but if it renders in software it should just
work. Downloads are Android `.so` files from the libretro buildbot; on iOS
the bundled engines cover you, and extra cores must be embedded as
self-built frameworks (see [AGENTS.md](AGENTS.md)).

## Show me

```php
use KevinBatdorf\RetroEmulator\{Emulator, System, Device};
use KevinBatdorf\RetroEmulator\Config\SfcConfig;
use KevinBatdorf\RetroEmulator\Buttons\SfcButton;

// A game on screen
$emu = Emulator::surface('main');
$emu->loadSystem(System::Sfc)->loadRom($romPath);

// The engine you fetched, by name — or set it once in config/retro-emulator.php
$emu->loadSystem(System::Sfc, new SfcConfig(backend: 'snes9x'));

// A core's own settings, validated against what it declares
$emu->loadSystem(System::Sfc, new SfcConfig(
    backend: 'snes9x',
    engineOptions: ['snes9x_region' => 'pal'],
));

// Everything is one call away
$emu->saveState()->toggleRewind()->setSpeed(2.0);
$emu->setShader($crtPreset);
$emu->addCheat('7E0010:01');
$emu->connectDevice(1, Device::Gamepad)->press(SfcButton::A);
$emu->watchMemory(0x7EF340, length: 2);   // fires events as the game writes
```

Or skip PHP entirely:

```blade
<native:emulator name="main" system="sfc" :rom="$romPath" />
<native:dpad surface="main" class="w-36 h-36" />
```

Full API, events, elements, and engine internals: [AGENTS.md](AGENTS.md).
