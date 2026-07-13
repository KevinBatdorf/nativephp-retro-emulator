## kevinbatdorf/retro-emulator

Native retro-game emulation (ares core) for NativePHP Mobile **v4 SuperNative**. The emulator
renders on a native surface (`GLSurfaceView`/`MTKView`) positioned by the EDGE layout engine;
PHP drives the lifecycle and reads emulated RAM live. There is **no JavaScript API** — all
control is PHP-side.

### Installation

```bash
composer require kevinbatdorf/retro-emulator
php artisan vendor:publish --tag=nativephp-plugins-provider
php artisan native:plugin:register kevinbatdorf/retro-emulator
php artisan native:install
```

The plugin never compiles into a build unless it is listed in
`App\Providers\NativeServiceProvider::plugins()` — `native:plugin:register` does that.

### Declaring the surface

The emulator is an EDGE element and must exist in the rendered layout before any bridge call:

@verbatim
<code-snippet name="Emulator surface in an EDGE view" lang="blade">
<native:column class="flex-1">
    <native:emulator name="main" class="flex-1" />
</native:column>
</code-snippet>
@endverbatim

### PHP Usage

Use the `KevinBatdorf\RetroEmulator\Facades\Emulator` facade. **Boot after first render, not
in `mount()`** — the native surface is created when the screen first renders, so bridge calls
from `mount()` cannot find it. Boot from a `#[Poll]` tick or any post-render interaction.

@verbatim
<code-snippet name="Boot and drive the emulator" lang="php">
use KevinBatdorf\RetroEmulator\Facades\Emulator;

Emulator::boot('main')
    ->loadSystem('sfc', config: ['autoSave' => true])
    ->loadRom(storage_path('app/roms/game.sfc'));

$bytes = Emulator::readMemory(0x7E0010);   // sync WRAM read, returns int[]
Emulator::watchMemory([0x7E0010]);         // MemoryChanged event on change
Emulator::pause();
Emulator::stateSave(slot: 1);
</code-snippet>
@endverbatim

Supported systems in the current build: `fc`, `sfc`, `gb`, `md` (`Emulator::getSystems()`
reports `supported` per system). Memory addresses are console bus addresses; each system
exposes its work-RAM window (sfc `0x7E0000`–`0x7FFFFF`, fc `0x0000`–`0x07FF`,
gb `0xC000`–`0xDFFF`, md `0xFF0000`–`0xFFFFFF`).

### Events

All under `KevinBatdorf\RetroEmulator\Events`: `EmulatorStarted` (first rendered frame after
`loadRom()`), `EmulatorStopped`, `EmulatorPaused`, `EmulatorResumed`, `MemoryRead` (response
to `readMemoryAsync()`), `MemoryChanged` (watched address changed), `EmulatorError` (runtime
failures — call-site failures come back inline as bridge errors instead).

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

Everything the API exposes works on Android. `setShader` applies librashader
`.slangp` presets (a preset that fails to load reports an `EmulatorError`,
`SHADER_FAILED`); `setInputMapping` merges a per-port controller remap
(`['a' => 'b', 'b' => 'a']` swaps A and B; unknown buttons throw). On iOS both
of these await the iOS host renderer and return `NOT_IMPLEMENTED` until then.
