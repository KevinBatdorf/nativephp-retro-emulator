<?php

namespace KevinBatdorf\RetroEmulator;

class Emulator
{
    private string $surface = 'main';

    /**
     * Bind to the named surface declared by <native:emulator name="..." />.
     * Returns a new instance so multiple surfaces can coexist.
     */
    public function boot(string $surface): static
    {
        $instance = new static;
        $instance->surface = $surface;

        if (function_exists('nativephp_call')) {
            nativephp_call('Emulator.Boot', json_encode(['surface' => $surface]));
        }

        return $instance;
    }

    /**
     * Initialise ares core for a system with system-level config.
     * This is the expensive call; call once per system, then swap ROMs via loadRom().
     *
     * @param  array{
     *     renderer?: 'opengl'|'vulkan',
     *     biosPath?: string|null,
     *     autoSave?: bool,
     *     runAhead?: int,
     *     rewind?: bool,
     *     rewindBufferSeconds?: int,
     *     speed?: float,
     * }  $config
     */
    public function loadSystem(string $system, array $config = []): static
    {
        if (function_exists('nativephp_call')) {
            nativephp_call('Emulator.LoadSystem', json_encode([
                'surface' => $this->surface,
                'system' => $system,
                'config' => $config,
            ]));
        }

        return $this;
    }

    /**
     * Start emulation with ROM at the given path.
     * Fire-and-forget — status goes to 'loading'; EmulatorStarted fires on first frame.
     * Call again without reinitialising the system to swap ROMs.
     */
    public function loadRom(string $path): static
    {
        if (function_exists('nativephp_call')) {
            nativephp_call('Emulator.LoadRom', json_encode([
                'surface' => $this->surface,
                'path' => $path,
            ]));
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

    public function stateSave(int|string $slot = 1): static
    {
        if (function_exists('nativephp_call')) {
            nativephp_call('Emulator.StateSave', json_encode([
                'surface' => $this->surface,
                'slot' => $slot,
            ]));
        }

        return $this;
    }

    public function stateLoad(int|string $slot = 1): static
    {
        if (function_exists('nativephp_call')) {
            nativephp_call('Emulator.StateLoad', json_encode([
                'surface' => $this->surface,
                'slot' => $slot,
            ]));
        }

        return $this;
    }

    public function undoStateSave(): static
    {
        if (function_exists('nativephp_call')) {
            nativephp_call('Emulator.UndoStateSave', json_encode(['surface' => $this->surface]));
        }

        return $this;
    }

    public function undoStateLoad(): static
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
     * Register/merge address watches. Native checks every frame and fires
     * MemoryChanged only when the value at that address actually changes.
     * Watches are cleared automatically when loadRom() is called.
     *
     * @param  array<int|array{address: int, length: int}>  $addresses
     */
    public function watchMemory(array $addresses): static
    {
        if (function_exists('nativephp_call')) {
            nativephp_call('Emulator.WatchMemory', json_encode([
                'surface' => $this->surface,
                'addresses' => $addresses,
            ]));
        }

        return $this;
    }

    /**
     * @param  int[]  $addresses
     */
    public function unwatchMemory(array $addresses): static
    {
        if (function_exists('nativephp_call')) {
            nativephp_call('Emulator.UnwatchMemory', json_encode([
                'surface' => $this->surface,
                'addresses' => $addresses,
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

    /**
     * @param  array{volume?: int, balance?: int}  $options
     */
    public function setAudio(array $options): static
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
     * @param  array{
     *     luminance?: int,
     *     saturation?: int,
     *     gamma?: float,
     *     colorEmulation?: bool,
     *     colorBleed?: bool,
     *     deepBlackBoost?: bool,
     *     interframeBlending?: bool,
     *     overscan?: bool,
     *     pixelAccuracy?: bool,
     * }  $options
     */
    public function setVideo(array $options): static
    {
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
     * speed (0.25–4.0) is live. runAhead and rewind are NOT implemented in
     * v1 — requesting a non-default value returns a NOT_IMPLEMENTED bridge
     * error.
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

    /** NOT implemented in v1 — the bridge returns a NOT_IMPLEMENTED error. */
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

    public function pressButton(int $port, string $button): static
    {
        if (function_exists('nativephp_call')) {
            nativephp_call('Emulator.PressButton', json_encode([
                'surface' => $this->surface,
                'port' => $port,
                'button' => $button,
            ]));
        }

        return $this;
    }

    public function releaseButton(int $port, string $button): static
    {
        if (function_exists('nativephp_call')) {
            nativephp_call('Emulator.ReleaseButton', json_encode([
                'surface' => $this->surface,
                'port' => $port,
                'button' => $button,
            ]));
        }

        return $this;
    }

    /**
     * Atomically merge multiple button states into the input state map.
     *
     * @param  array<string, bool>  $state
     */
    public function setButtons(int $port, array $state): static
    {
        if (function_exists('nativephp_call')) {
            nativephp_call('Emulator.SetButtons', json_encode([
                'surface' => $this->surface,
                'port' => $port,
                'state' => $state,
            ]));
        }

        return $this;
    }

    public function screenshot(): static
    {
        if (function_exists('nativephp_call')) {
            nativephp_call('Emulator.Screenshot', json_encode(['surface' => $this->surface]));
        }

        return $this;
    }

    /** @return 'stopped'|'loading'|'running'|'paused' */
    public function getStatus(): string
    {
        if (function_exists('nativephp_call')) {
            $result = nativephp_call('Emulator.GetStatus', json_encode(['surface' => $this->surface]));

            if ($result) {
                $decoded = json_decode($result, true);

                return $decoded['status'] ?? 'stopped';
            }
        }

        return 'stopped';
    }

    public function getPorts(): array
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

    public function getRegion(): string
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
     * Return all supported systems as rich objects.
     *
     * @return array<int, array{id: string, name: string, biosRequired: bool, stable: bool}>
     */
    public static function getSystems(): array
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
