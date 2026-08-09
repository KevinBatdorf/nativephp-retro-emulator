<?php

namespace KevinBatdorf\RetroEmulator;

use KevinBatdorf\RetroEmulator\Backend;
use KevinBatdorf\RetroEmulator\Concerns\InteractsWithBridge;
use KevinBatdorf\RetroEmulator\Config\Config;
use KevinBatdorf\RetroEmulator\Config\SystemConfig;

class Emulator
{
    use InteractsWithBridge;

    private string $surface = 'main';

    /**
     * Config keys fanned out to the AV setters instead of the staged system
     * config — including inputCapture, which the surface resolves at creation.
     */
    private const PRESENTATION_KEYS = [
        'luminance', 'saturation', 'gamma', 'colorBleed', 'overscan',
        'output', 'fixedScale', 'aspectCorrection',
        'volume', 'balance', 'rumble', 'shader', 'inputCapture',
    ];

    /**
     * Runtime handle for the named surface declared by
     * <native:emulator name="..." />. Returns a new instance so multiple
     * surfaces can coexist.
     */
    public static function surface(string $name = 'main'): static
    {
        $instance = new static;
        $instance->surface = $name;
        $instance->call('Emulator.Boot', ['surface' => $name]);

        return $instance;
    }

    /**
     * Declare the system and its config. Nothing boots until loadRom() —
     * every ROM load boots a fresh core with the region resolved from the
     * ROM itself, so PAL games run at PAL speed automatically. Use the
     * config's region/preferredRegions knobs to override.
     *
     * @param  SystemConfig|array{
     *     biosPath?: string|null,
     *     autoSave?: bool,
     *     runAhead?: int,
     *     rewind?: bool,
     *     rewindBufferSeconds?: int,
     *     speed?: float,
     *     dynamicRateControl?: bool,
     *     region?: string,
     *     preferredRegions?: string[],
     * }  $config
     */
    public function loadSystem(System|string $system, SystemConfig|array $config = []): static
    {
        $systemId = $system instanceof System ? $system->value : $system;

        $staged = $config instanceof Config
            ? array_diff_key($config->toArray(), array_flip(self::PRESENTATION_KEYS))
            : $config;

        // Engine resolution: an explicit backend is strict (must serve or
        // throw); the config map is a graceful preference list — engines
        // that aren't present are skipped, ending at the built-in engine.
        if (! isset($staged['backend'])) {
            $preferences = config('retro-emulator.backends')[$systemId] ?? [];

            if ($preferences !== [] && $preferences !== null) {
                $staged['backendPreferences'] = array_values(array_map(
                    fn (Backend|string $engine) => $engine instanceof Backend
                        ? $engine->value
                        : $engine,
                    (array) $preferences,
                ));
            }
        }
        $staged = array_filter($staged, fn ($value) => $value !== null);

        $result = $this->call('Emulator.LoadSystem', [
            'surface' => $this->surface,
            'system' => $systemId,
            'config' => $staged,
        ]);

        $preferred = $staged['backendPreferences'][0] ?? null;
        $actual = $result['backend'] ?? '';

        if ($preferred !== null && $actual !== '' && $actual !== $preferred && app()->bound('log')) {
            logger()->warning(
                "retro-emulator: {$systemId} is running '{$actual}' — the config prefers "
                ."'{$preferred}'. Run: php artisan retro-emulator:fetch-core {$preferred}"
            );
        }

        if ($config instanceof Config) {
            $this->applyPresentation($config);
        }

        return $this;
    }

    /**
     * Fan a config's presentation/AV knobs out to their own native channels,
     * which the staged system config doesn't carry.
     */
    private function applyPresentation(Config $config): void
    {
        $video = [
            $config->luminance, $config->saturation, $config->gamma,
            $config->colorBleed, $config->overscan, $config->output,
            $config->fixedScale, $config->aspectCorrection,
        ];
        if (array_filter($video, fn ($value) => $value !== null) !== []) {
            $this->setVideo(
                luminance: $config->luminance,
                saturation: $config->saturation,
                gamma: $config->gamma,
                colorBleed: $config->colorBleed,
                overscan: $config->overscan,
                output: $config->output,
                fixedScale: $config->fixedScale,
                aspectCorrection: $config->aspectCorrection,
            );
        }

        $audio = array_filter([
            'volume' => $config->volume,
            'balance' => $config->balance,
        ], fn ($value) => $value !== null);
        if ($audio !== []) {
            $this->setAudio($audio);
        }

        if ($config->rumble !== null) {
            $this->setRumble($config->rumble);
        }

        if ($config->shader !== null) {
            $this->setShader($config->shader);
        }
    }

    /**
     * Boot a ROM. Fire-and-forget — status goes to 'loading'; EmulatorStarted
     * fires on the first frame. Call again to swap ROMs without reinitialising
     * the system.
     *
     * Pass a path for a normal cartridge, or a slotted-media spec for a base
     * cartridge with inserted slots (SuFami Turbo):
     *
     *   $emu->loadRom('/roms/game.sfc');
     *   $emu->loadRom(['base' => '/roms/sufami.sfc', 'slotA' => '/roms/game.st']);
     *
     * The `base` is the cartridge; `slotA`/`slotB` are staged into its slots
     * before boot so the slot game runs directly (not the base's menu).
     *
     * @param  string|array{base: string, slotA?: string, slotB?: string}  $rom
     * @param  string|null  $savePath  Battery-save file prefix; null keeps the
     *                                 default per-surface location in app storage.
     */
    public function loadRom(string|array $rom, ?string $savePath = null): static
    {
        if (is_array($rom)) {
            foreach (['slotA' => 0, 'slotB' => 1] as $key => $index) {
                if (isset($rom[$key])) {
                    $this->call('Emulator.StageSlot', [
                        'surface' => $this->surface,
                        'index' => $index,
                        'path' => $rom[$key],
                    ]);
                }
            }
            $path = $rom['base'];
        } else {
            $path = $rom;
        }

        $this->call('Emulator.LoadRom', array_filter([
            'surface' => $this->surface,
            'path' => $path,
            'savePath' => $savePath,
        ], fn ($value) => $value !== null));

        return $this;
    }

    public function pause(): static
    {
        $this->call('Emulator.Pause', ['surface' => $this->surface]);

        return $this;
    }

    public function resume(): static
    {
        $this->call('Emulator.Resume', ['surface' => $this->surface]);

        return $this;
    }

    public function stop(): static
    {
        $this->call('Emulator.Stop', ['surface' => $this->surface]);

        return $this;
    }

    public function saveState(int|string $slot = 1): static
    {
        $this->call('Emulator.StateSave', [
            'surface' => $this->surface,
            'slot' => $slot,
        ]);

        return $this;
    }

    public function loadState(int|string $slot = 1): static
    {
        $this->call('Emulator.StateLoad', [
            'surface' => $this->surface,
            'slot' => $slot,
        ]);

        return $this;
    }

    public function undoSaveState(): static
    {
        $this->call('Emulator.UndoStateSave', ['surface' => $this->surface]);

        return $this;
    }

    public function undoLoadState(): static
    {
        $this->call('Emulator.UndoStateLoad', ['surface' => $this->surface]);

        return $this;
    }

    /**
     * Synchronous RAM read; the async variant reports through MemoryRead instead.
     *
     * @return int[]
     */
    public function readMemory(int $address, int $length = 1): array
    {
        $result = $this->call('Emulator.ReadMemory', [
            'surface' => $this->surface,
            'address' => $address,
            'length' => $length,
        ]);

        return $result['bytes'] ?? [];
    }

    /**
     * Async RAM read — dispatches MemoryRead event with the result.
     */
    public function readMemoryAsync(int $address, int $length = 1): static
    {
        $this->call('Emulator.ReadMemoryAsync', [
            'surface' => $this->surface,
            'address' => $address,
            'length' => $length,
        ]);

        return $this;
    }

    /**
     * Write bytes to WRAM. Throws WRITE_FAILED (an EmulatorException) if the
     * address is out of range or no core is running.
     *
     * @param  int[]  $bytes
     */
    public function writeMemory(int $address, array $bytes): static
    {
        $this->call('Emulator.WriteMemory', [
            'surface' => $this->surface,
            'address' => $address,
            'bytes' => $bytes,
        ]);

        return $this;
    }

    /**
     * Register/merge an address watch. Native checks every frame and fires
     * MemoryChanged only when the value at that address actually changes.
     * Chain per address; watches survive ROM swaps until cleared explicitly.
     */
    public function watchMemory(int $address, int $length = 1): static
    {
        $this->call('Emulator.WatchMemory', [
            'surface' => $this->surface,
            'addresses' => [['address' => $address, 'length' => $length]],
        ]);

        return $this;
    }

    public function unwatchMemory(int $address): static
    {
        $this->call('Emulator.UnwatchMemory', [
            'surface' => $this->surface,
            'addresses' => [$address],
        ]);

        return $this;
    }

    public function clearMemoryWatches(): static
    {
        $this->call('Emulator.ClearMemoryWatches', ['surface' => $this->surface]);

        return $this;
    }

    /** Per-emulator gain (0–100), not device volume. */
    public function setVolume(int $volume): static
    {
        return $this->setAudio(['volume' => $volume]);
    }

    /** Stereo balance: −100 full left … +100 full right. */
    public function setBalance(int $balance): static
    {
        return $this->setAudio(['balance' => $balance]);
    }

    /** @param  array{volume?: int, balance?: int}  $options */
    private function setAudio(array $options): static
    {
        $this->call('Emulator.SetAudio', [
            'surface' => $this->surface,
            'options' => $options,
        ]);

        return $this;
    }

    /**
     * Merge GLOBAL display settings — presentation knobs that mean the same
     * thing on every system. Omitted options keep their current values and
     * persist across ROM/system swaps. Overscan borders are trimmed by default;
     * overscan: true shows the full canvas.
     *
     * luminance, saturation and gamma are whole PERCENTAGES where 100 leaves the
     * picture untouched — gamma spans 50–200, the others 0–100. Out-of-range
     * values are rejected, not clamped.
     */
    public function setVideo(
        ?int $luminance = null,
        ?int $saturation = null,
        ?int $gamma = null,
        ?bool $colorBleed = null,
        ?bool $overscan = null,
        ?VideoOutput $output = null,
        ?int $fixedScale = null,
        ?AspectCorrection $aspectCorrection = null,
    ): static {
        $options = array_filter(compact(
            'luminance', 'saturation', 'gamma', 'colorBleed', 'overscan',
        ), fn ($value) => $value !== null);

        if ($output !== null) {
            $options['output'] = $output->value;
        }
        if ($fixedScale !== null) {
            $options['fixedScale'] = $fixedScale;
        }
        if ($aspectCorrection !== null) {
            $options['aspectCorrection'] = $aspectCorrection->value;
        }

        $this->call('Emulator.SetVideo', [
            'surface' => $this->surface,
            'options' => $options,
        ]);

        return $this;
    }

    /**
     * Merge general live options. All keys merge into current state.
     *
     * speed (0.25–4.0) scales the tick budget. runAhead accepts 0 or 1 —
     * ares supports exactly one hidden frame, halving perceived input lag at
     * 2× emulation cost. rewind toggles snapshot capture (rewindBufferSeconds
     * sizes the history, default ~16.7 s); play it back with toggleRewind().
     *
     * engineOptions applies engine-declared options (libretro core options)
     * to the running core, which re-reads them between frames — same
     * validation and use-at-your-own-risk contract as the loadSystem config:
     * an undeclared key or value throws UNSUPPORTED_OPTION, never a silent
     * no-op. Enumerate what's declared with engineOptions().
     *
     * @param  array{speed?: float, runAhead?: int, rewind?: bool, rewindBufferSeconds?: int, engineOptions?: array<string, string>}  $options
     */
    public function configure(array $options): static
    {
        $this->call('Emulator.Configure', [
            'surface' => $this->surface,
            'options' => $options,
        ]);

        return $this;
    }

    /**
     * Enter/exit rewind playback (5× the capture rate, ares desktop
     * semantics). Play resumes automatically when history runs out. Requires
     * rewind capture enabled via loadSystem() config or configure() —
     * toggling while disabled throws REWIND_DISABLED.
     */
    public function toggleRewind(): static
    {
        $this->call('Emulator.ToggleRewind', ['surface' => $this->surface]);

        return $this;
    }

    /**
     * Merge system-specific options (e.g. ['deepBlackBoost' => true] for SNES).
     *
     * @param  array<string, mixed>  $options
     */
    public function setSystemOptions(array $options): static
    {
        $this->call('Emulator.SetSystemOptions', [
            'surface' => $this->surface,
            'options' => $options,
        ]);

        return $this;
    }

    /** Live emulation speed (0.25–4.0); 1.0 is native speed. */
    public function setSpeed(float $speed): static
    {
        return $this->configure(['speed' => $speed]);
    }

    public function fastForward(bool $enabled): static
    {
        $this->call('Emulator.FastForward', [
            'surface' => $this->surface,
            'enabled' => $enabled,
        ]);

        return $this;
    }

    /**
     * Register (or swap) the controller on a port and return its {@see Controller}
     * handle — controllers are explicit, never auto-allocated. Drive it through
     * the handle (press/release/setButtons/setAxis/aimAt/remap). The registration
     * persists across loadRom.
     *
     * Returns one Controller, for $port. A multitap connects here too (you get
     * its base port); use {@see connectMultitap()} for a handle per player.
     *
     * An unsupported device, a bad port, or no staged system is a programmer
     * error and throws EmulatorException synchronously.
     */
    public function connectDevice(int $port, Device|string $device): Controller
    {
        $this->call('Emulator.ConnectDevice', [
            'surface' => $this->surface,
            'port' => $port,
            'device' => $device instanceof Device ? $device->value : $device,
        ]);

        return new Controller($this->surface, $port);
    }

    /**
     * Connect a multitap on $port and return one {@see Controller} per player it
     * fans out to (Super Multitap → four). For single-player devices use
     * {@see connectDevice()}.
     *
     * Same error contract as connectDevice(): a bad port, unsupported device, or
     * no staged system throws EmulatorException synchronously.
     *
     * @return Controller[]
     */
    public function connectMultitap(int $port, Device|string $device): array
    {
        $result = $this->call('Emulator.ConnectDevice', [
            'surface' => $this->surface,
            'port' => $port,
            'device' => $device instanceof Device ? $device->value : $device,
        ]);

        $ports = $result['ports'] ?? [$port];

        return array_map(fn (int $p) => new Controller($this->surface, $p), $ports);
    }

    /**
     * Handle for the controller already registered on a port (e.g. connected
     * declaratively, or earlier in this session). A thin handle — it does not
     * verify a device is present; driving an empty port throws UNKNOWN_BUTTON.
     */
    public function getDevice(int $port): Controller
    {
        return new Controller($this->surface, $port);
    }

    /**
     * Gate rumble forwarding: while enabled, motor state published by the
     * emulated hardware (SFC Rumble Gamepad, GB MBC5 rumble carts) drives
     * the device vibrator/haptics.
     */
    public function setRumble(bool $enabled): static
    {
        $this->call('Emulator.SetRumble', [
            'surface' => $this->surface,
            'enabled' => $enabled,
        ]);

        return $this;
    }

    /**
     * Apply a librashader `.slangp` preset by path; pass null to clear it
     * (passthrough). Discover presets with {@see Shaders::in()}. A preset that
     * fails to load surfaces as an EmulatorError event (SHADER_FAILED), not a
     * throw — the command stays fluent.
     */
    public function setShader(?string $path): static
    {
        $this->call('Emulator.SetShader', [
            'surface' => $this->surface,
            'path' => $path,
        ]);

        return $this;
    }

    /**
     * Register a cheat in ares' raw format: hex "ADDR:VALUE" pairs joined with
     * '+' (e.g. "7E0010:01+7E0011:FF"). The value overrides every CPU read of
     * the address while active. Re-adding a code replaces it; cheats clear
     * automatically when a new ROM loads. Game Genie / GameShark codes are not
     * parsed (unsupported upstream in ares) — convert them first.
     */
    public function addCheat(string $code, string $description = ''): static
    {
        $this->call('Emulator.AddCheat', [
            'surface' => $this->surface,
            'code' => $code,
            'description' => $description,
        ]);

        return $this;
    }

    public function removeCheat(string $code): static
    {
        $this->call('Emulator.RemoveCheat', [
            'surface' => $this->surface,
            'code' => $code,
        ]);

        return $this;
    }

    public function clearCheats(): static
    {
        $this->call('Emulator.ClearCheats', ['surface' => $this->surface]);

        return $this;
    }

    /**
     * Register every enabled cheat from a desktop-ares .cheats.bml file, so
     * cheat lists are interchangeable with desktop ares. Disabled or codeless
     * entries are skipped.
     *
     * @throws \RuntimeException When the file cannot be read.
     */
    public function loadCheatsFile(string $path): static
    {
        if (! is_readable($path)) {
            throw new \RuntimeException("Cheats file not readable: {$path}");
        }

        foreach ($this->parseCheatsBml((string) file_get_contents($path)) as $cheat) {
            $this->addCheat($cheat['code'], $cheat['description']);
        }

        return $this;
    }

    /**
     * Parse desktop-ares .cheats.bml content: top-level `cheat` nodes with
     * indented `description:` / `code:` / `enabled:` children.
     *
     * @return list<array{code: string, description: string}>
     */
    private function parseCheatsBml(string $bml): array
    {
        $cheats = [];
        $current = null;

        $flush = function () use (&$cheats, &$current): void {
            if ($current && $current['enabled'] && $current['code'] !== '') {
                $cheats[] = ['code' => $current['code'], 'description' => $current['description']];
            }
            $current = null;
        };

        foreach (preg_split('/\r?\n/', $bml) as $line) {
            if (trim($line) === 'cheat') {
                $flush();
                $current = ['description' => '', 'code' => '', 'enabled' => false];

                continue;
            }

            if ($current === null || ! preg_match('/^\s+(description|code|enabled):\s*(.*)$/', $line, $m)) {
                continue;
            }

            $current[$m[1]] = $m[1] === 'enabled' ? $m[2] === 'true' : $m[2];
        }

        $flush();

        return $cheats;
    }

    /** Capture the current frame as a PNG; returns its path, or null on failure. */
    public function screenshot(): ?string
    {
        $result = $this->call('Emulator.Screenshot', ['surface' => $this->surface]);

        return $result['path'] ?? null;
    }

    public function status(): Status
    {
        $result = $this->call('Emulator.GetStatus', ['surface' => $this->surface]);

        return Status::tryFrom($result['status'] ?? '') ?? Status::Stopped;
    }

    /**
     * Which renderer the running core actually bound — read back from the
     * core, not from what was requested. Null before a boot and on cores with
     * a single renderer (NES, GB/GBC, Mega Drive). Boot-only: change it via
     * the LoadSystem config, then reboot.
     */
    public function accuracy(): ?Accuracy
    {
        $result = $this->call('Emulator.GetStatus', ['surface' => $this->surface]);

        return Accuracy::tryFrom($result['accuracy'] ?? '');
    }

    /**
     * The controller ports of the staged system: the device connected to each
     * (null if none), its button + axis names, and which devices the port
     * supports (for connectDevice).
     *
     * @return array<int, array{port: int, device: ?string, buttons: string[], axes: string[], supported: string[]}>
     */
    public function ports(): array
    {
        $result = $this->call('Emulator.GetPorts', ['surface' => $this->surface]);

        return $result['ports'] ?? [];
    }

    public function region(): string
    {
        $result = $this->call('Emulator.GetRegion', ['surface' => $this->surface]);

        return $result['region'] ?? '';
    }

    /**
     * The engine-declared option schema of the staged/active engine — the
     * keys and values legal in `engineOptions`. Empty for the bundled
     * engines (ares, SameBoy, mGBA), whose settings are the typed config;
     * a libretro core reports every option it declares.
     *
     * @return array<int, array{key: string, choices: string[], default: string, current: string}>
     */
    public function engineOptions(): array
    {
        $result = $this->call('Emulator.GetEngineOptions', ['surface' => $this->surface]);

        return $result['options'] ?? [];
    }

    /**
     * Return all systems as rich objects. `supported` reflects whether the
     * system's core is compiled into this build's native library. `backends`
     * lists the engines serving the system in this build (bring-your-own
     * libretro cores appear once fetched), and `capabilities` carries one
     * object per backend: videoSettings/rumble/serialize/cheats/memoryAccess/
     * slottedMedia/multitap/mouse flags plus `toggles` (the boolean
     * setSystemOptions keys this engine+system pair accepts) and
     * `bootOptions` (boot-only booleans such as pixelAccuracy, rawAudio).
     *
     * GetSystems has no error path, so this static query talks to the bridge
     * directly rather than through the instance-scoped call() router.
     *
     * @return array<int, array{id: string, name: string, stable: bool, supported: bool, backends: array<int, string>, capabilities: array<string, array<string, mixed>>}>
     */
    public static function systems(): array
    {
        if (function_exists('nativephp_call')) {
            $result = nativephp_call('Emulator.GetSystems', '{}');

            if ($result) {
                $decoded = json_decode($result, true);

                return $decoded['systems'] ?? [];
            }
        }

        return [];
    }

    /**
     * One backend's capability object for a system, from the same data
     * systems() returns — [] when the pair is unknown (off-device, a system
     * this build lacks, or an engine that does not serve it).
     *
     * @return array{videoSettings?: bool, rumble?: bool, serialize?: bool, cheats?: bool, memoryAccess?: bool, slottedMedia?: bool, multitap?: bool, mouse?: bool, toggles?: array<int, string>, bootOptions?: array<int, string>}
     */
    public static function capabilities(string $system, string $backend): array
    {
        foreach (self::systems() as $entry) {
            if (($entry['id'] ?? '') === $system) {
                return $entry['capabilities'][$backend] ?? [];
            }
        }

        return [];
    }

    /**
     * Hardware controllers the OS currently reports as connected — a
     * handheld's built-in pad, paired Bluetooth gamepads. Names are the
     * platform's device names; empty when none are connected. No surface
     * required.
     *
     * @return array<int, string>
     */
    public static function inputDevices(): array
    {
        if (function_exists('nativephp_call')) {
            $result = nativephp_call('Emulator.GetInputDevices', '{}');

            if ($result) {
                $decoded = json_decode($result, true);

                return $decoded['devices'] ?? [];
            }
        }

        return [];
    }
}
