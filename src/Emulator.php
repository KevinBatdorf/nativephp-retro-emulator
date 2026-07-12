<?php

namespace KevinBatdorf\RetroEmulator;

use KevinBatdorf\RetroEmulator\Config\SystemConfig;

class Emulator
{
    private string $surface = 'main';

    /**
     * Runtime handle for the named surface declared by
     * <native:emulator name="..." />. Returns a new instance so multiple
     * surfaces can coexist.
     */
    public static function surface(string $name = 'main'): static
    {
        $instance = new static;
        $instance->surface = $name;

        if (function_exists('nativephp_call')) {
            nativephp_call('Emulator.Boot', json_encode(['surface' => $name]));
        }

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
        if (function_exists('nativephp_call')) {
            nativephp_call('Emulator.LoadSystem', json_encode([
                'surface' => $this->surface,
                'system' => $system instanceof System ? $system->value : $system,
                'config' => $config instanceof SystemConfig ? $config->toArray() : $config,
            ]));
        }

        return $this;
    }

    /**
     * Start emulation with ROM at the given path.
     * Fire-and-forget — status goes to 'loading'; EmulatorStarted fires on first frame.
     * Call again without reinitialising the system to swap ROMs.
     *
     * @param  string|null  $savePath  Battery-save file prefix; null keeps the
     *                                 default per-surface location in app storage.
     */
    public function loadRom(string $path, ?string $savePath = null): static
    {
        if (function_exists('nativephp_call')) {
            nativephp_call('Emulator.LoadRom', json_encode(array_filter([
                'surface' => $this->surface,
                'path' => $path,
                'savePath' => $savePath,
            ], fn ($value) => $value !== null)));
        }

        return $this;
    }

    public function pause(): static
    {
        if (function_exists('nativephp_call')) {
            nativephp_call('Emulator.Pause', json_encode(['surface' => $this->surface]));
        }

        return $this;
    }

    public function resume(): static
    {
        if (function_exists('nativephp_call')) {
            nativephp_call('Emulator.Resume', json_encode(['surface' => $this->surface]));
        }

        return $this;
    }

    public function stop(): static
    {
        if (function_exists('nativephp_call')) {
            nativephp_call('Emulator.Stop', json_encode(['surface' => $this->surface]));
        }

        return $this;
    }

    public function saveState(int|string $slot = 1): static
    {
        if (function_exists('nativephp_call')) {
            nativephp_call('Emulator.StateSave', json_encode([
                'surface' => $this->surface,
                'slot' => $slot,
            ]));
        }

        return $this;
    }

    public function loadState(int|string $slot = 1): static
    {
        if (function_exists('nativephp_call')) {
            nativephp_call('Emulator.StateLoad', json_encode([
                'surface' => $this->surface,
                'slot' => $slot,
            ]));
        }

        return $this;
    }

    public function undoSaveState(): static
    {
        if (function_exists('nativephp_call')) {
            nativephp_call('Emulator.UndoStateSave', json_encode(['surface' => $this->surface]));
        }

        return $this;
    }

    public function undoLoadState(): static
    {
        if (function_exists('nativephp_call')) {
            nativephp_call('Emulator.UndoStateLoad', json_encode(['surface' => $this->surface]));
        }

        return $this;
    }

    /**
     * Synchronous RAM read — returns bytes directly.
     *
     * @return int[]
     */
    public function readMemory(int $address, int $length = 1): array
    {
        if (function_exists('nativephp_call')) {
            $result = nativephp_call('Emulator.ReadMemory', json_encode([
                'surface' => $this->surface,
                'address' => $address,
                'length' => $length,
            ]));

            if ($result) {
                $decoded = json_decode($result, true);

                return $decoded['bytes'] ?? [];
            }
        }

        return [];
    }

    /**
     * Async RAM read — dispatches MemoryRead event with the result.
     */
    public function readMemoryAsync(int $address, int $length = 1): static
    {
        if (function_exists('nativephp_call')) {
            nativephp_call('Emulator.ReadMemoryAsync', json_encode([
                'surface' => $this->surface,
                'address' => $address,
                'length' => $length,
            ]));
        }

        return $this;
    }

    /**
     * @param  int[]  $bytes
     */
    public function writeMemory(int $address, array $bytes): static
    {
        if (function_exists('nativephp_call')) {
            nativephp_call('Emulator.WriteMemory', json_encode([
                'surface' => $this->surface,
                'address' => $address,
                'bytes' => $bytes,
            ]));
        }

        return $this;
    }

    /**
     * Register/merge an address watch. Native checks every frame and fires
     * MemoryChanged only when the value at that address actually changes.
     * Chain per address; watches survive ROM swaps until cleared explicitly.
     */
    public function watchMemory(int $address, int $length = 1): static
    {
        if (function_exists('nativephp_call')) {
            nativephp_call('Emulator.WatchMemory', json_encode([
                'surface' => $this->surface,
                'addresses' => [['address' => $address, 'length' => $length]],
            ]));
        }

        return $this;
    }

    public function unwatchMemory(int $address): static
    {
        if (function_exists('nativephp_call')) {
            nativephp_call('Emulator.UnwatchMemory', json_encode([
                'surface' => $this->surface,
                'addresses' => [$address],
            ]));
        }

        return $this;
    }

    public function clearMemoryWatches(): static
    {
        if (function_exists('nativephp_call')) {
            nativephp_call('Emulator.ClearMemoryWatches', json_encode(['surface' => $this->surface]));
        }

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
        if (function_exists('nativephp_call')) {
            nativephp_call('Emulator.SetAudio', json_encode([
                'surface' => $this->surface,
                'options' => $options,
            ]));
        }

        return $this;
    }

    /**
     * Merge GLOBAL display settings — presentation knobs that mean the same
     * thing on every system. Omitted options keep their current values and
     * persist across ROM/system swaps. Overscan borders are trimmed by default;
     * overscan: true shows the full canvas. Presentation follows ares desktop's
     * Video settings: output VideoOutput::Scale (best-fit, default), Integer
     * (largest whole multiple), IntegerFixed (exactly fixedScale×), or Stretch;
     * aspectCorrection AspectCorrection::Standard (default), None (square
     * pixels), or Anamorphic (force 4:3).
     *
     * Per-system emulation toggles (Color Emulation, Deep Black Boost,
     * Interframe Blending) are not here — they only exist on the cores that
     * declare them, so they live on the per-system config classes and
     * setSystemOptions().
     */
    public function setVideo(
        ?int $luminance = null,
        ?int $saturation = null,
        ?float $gamma = null,
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

        if (function_exists('nativephp_call')) {
            nativephp_call('Emulator.SetVideo', json_encode([
                'surface' => $this->surface,
                'options' => $options,
            ]));
        }

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
     * @param  array{speed?: float, runAhead?: int, rewind?: bool, rewindBufferSeconds?: int}  $options
     */
    public function configure(array $options): static
    {
        if (function_exists('nativephp_call')) {
            nativephp_call('Emulator.Configure', json_encode([
                'surface' => $this->surface,
                'options' => $options,
            ]));
        }

        return $this;
    }

    /**
     * Enter/exit rewind playback (5× the capture rate, ares desktop
     * semantics). Play resumes automatically when history runs out. Requires
     * rewind capture enabled via loadSystem() config or configure() —
     * toggling while disabled returns a REWIND_DISABLED bridge error.
     */
    public function toggleRewind(): static
    {
        if (function_exists('nativephp_call')) {
            nativephp_call('Emulator.ToggleRewind', json_encode(['surface' => $this->surface]));
        }

        return $this;
    }

    /**
     * Merge system-specific options (e.g. ['expansionPak' => true] for N64).
     *
     * @param  array<string, mixed>  $options
     */
    public function setSystemOptions(array $options): static
    {
        if (function_exists('nativephp_call')) {
            nativephp_call('Emulator.SetSystemOptions', json_encode([
                'surface' => $this->surface,
                'options' => $options,
            ]));
        }

        return $this;
    }

    /** Live emulation speed (0.25–4.0); 1.0 is native speed. */
    public function setSpeed(float $speed): static
    {
        return $this->configure(['speed' => $speed]);
    }

    public function fastForward(bool $enabled): static
    {
        if (function_exists('nativephp_call')) {
            nativephp_call('Emulator.FastForward', json_encode([
                'surface' => $this->surface,
                'enabled' => $enabled,
            ]));
        }

        return $this;
    }

    /**
     * Merge controller input mappings for a port.
     *
     * NOT implemented in v1 — hardware mappings are fixed per system; the
     * bridge returns a NOT_IMPLEMENTED error.
     *
     * @param  array<string, string>  $mappings
     */
    public function setInputMapping(int $port, array $mappings): static
    {
        if (function_exists('nativephp_call')) {
            nativephp_call('Emulator.SetInputMapping', json_encode([
                'surface' => $this->surface,
                'port' => $port,
                'mappings' => $mappings,
            ]));
        }

        return $this;
    }

    /**
     * Gate rumble forwarding: while enabled, motor state published by the
     * emulated hardware (SFC Rumble Gamepad, GB MBC5 rumble carts, N64
     * Rumble Pak) drives the device vibrator/haptics. The bridge response
     * reports hasVibrator so apps can hide their rumble toggle on devices
     * without a motor.
     */
    public function setRumble(bool $enabled): static
    {
        if (function_exists('nativephp_call')) {
            nativephp_call('Emulator.SetRumble', json_encode([
                'surface' => $this->surface,
                'enabled' => $enabled,
            ]));
        }

        return $this;
    }

    /**
     * Load a librashader-compatible shader preset by path. Pass null to clear.
     *
     * NOT implemented in v1 — loading a shader returns a NOT_IMPLEMENTED
     * bridge error (clearing succeeds; there is never an active shader).
     */
    public function setShader(?string $path): static
    {
        if (function_exists('nativephp_call')) {
            nativephp_call('Emulator.SetShader', json_encode([
                'surface' => $this->surface,
                'path' => $path,
            ]));
        }

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
        if (function_exists('nativephp_call')) {
            nativephp_call('Emulator.AddCheat', json_encode([
                'surface' => $this->surface,
                'code' => $code,
                'description' => $description,
            ]));
        }

        return $this;
    }

    public function removeCheat(string $code): static
    {
        if (function_exists('nativephp_call')) {
            nativephp_call('Emulator.RemoveCheat', json_encode([
                'surface' => $this->surface,
                'code' => $code,
            ]));
        }

        return $this;
    }

    public function clearCheats(): static
    {
        if (function_exists('nativephp_call')) {
            nativephp_call('Emulator.ClearCheats', json_encode(['surface' => $this->surface]));
        }

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

    public function pressButton(int $port, \BackedEnum|string $button): static
    {
        if (function_exists('nativephp_call')) {
            nativephp_call('Emulator.PressButton', json_encode([
                'surface' => $this->surface,
                'port' => $port,
                'button' => $button instanceof \BackedEnum ? $button->value : $button,
            ]));
        }

        return $this;
    }

    public function releaseButton(int $port, \BackedEnum|string $button): static
    {
        if (function_exists('nativephp_call')) {
            nativephp_call('Emulator.ReleaseButton', json_encode([
                'surface' => $this->surface,
                'port' => $port,
                'button' => $button instanceof \BackedEnum ? $button->value : $button,
            ]));
        }

        return $this;
    }

    /**
     * Press multiple buttons atomically — all land in the same input frame,
     * unlike chained pressButton() calls. Release them individually.
     *
     * @param  array<\BackedEnum|string>  $buttons
     */
    public function pressButtons(int $port, array $buttons): static
    {
        $state = [];
        foreach ($buttons as $button) {
            $state[$button instanceof \BackedEnum ? $button->value : $button] = true;
        }

        if (function_exists('nativephp_call')) {
            nativephp_call('Emulator.SetButtons', json_encode([
                'surface' => $this->surface,
                'port' => $port,
                'state' => $state,
            ]));
        }

        return $this;
    }

    /** Capture the current frame as a PNG; returns its path, or null on failure. */
    public function screenshot(): ?string
    {
        if (function_exists('nativephp_call')) {
            $result = nativephp_call('Emulator.Screenshot', json_encode(['surface' => $this->surface]));

            if ($result) {
                $decoded = json_decode($result, true);

                return $decoded['path'] ?? null;
            }
        }

        return null;
    }

    public function status(): Status
    {
        if (function_exists('nativephp_call')) {
            $result = nativephp_call('Emulator.GetStatus', json_encode(['surface' => $this->surface]));

            if ($result) {
                $decoded = json_decode($result, true);

                return Status::tryFrom($decoded['status'] ?? '') ?? Status::Stopped;
            }
        }

        return Status::Stopped;
    }

    /** @return array<int, array{port: int, buttons: string[]}> */
    public function ports(): array
    {
        if (function_exists('nativephp_call')) {
            $result = nativephp_call('Emulator.GetPorts', json_encode(['surface' => $this->surface]));

            if ($result) {
                $decoded = json_decode($result, true);

                return $decoded['ports'] ?? [];
            }
        }

        return [];
    }

    public function region(): string
    {
        if (function_exists('nativephp_call')) {
            $result = nativephp_call('Emulator.GetRegion', json_encode(['surface' => $this->surface]));

            if ($result) {
                $decoded = json_decode($result, true);

                return $decoded['region'] ?? '';
            }
        }

        return '';
    }

    /**
     * Return all ares systems as rich objects. `supported` reflects whether
     * the system's core is compiled into this build's native library.
     *
     * @return array<int, array{id: string, name: string, biosRequired: bool, stable: bool, supported: bool}>
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
}
