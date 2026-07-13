<?php

namespace KevinBatdorf\RetroEmulator\Elements;

use KevinBatdorf\RetroEmulator\Config\Config;
use KevinBatdorf\RetroEmulator\Config\SystemConfig;
use KevinBatdorf\RetroEmulator\InputCapture;
use KevinBatdorf\RetroEmulator\System;
use Native\Mobile\Edge\CallbackRegistry;
use Native\Mobile\Edge\Element;

/**
 * EDGE element behind `<native:emulator />`. Beyond name/z-index it can carry
 * its own setup — system, config, rom — so native boots the core when the
 * surface mounts and tears it down when it leaves the tree, with no PHP
 * round-trip. All setup attributes are optional; an app can instead drive
 * everything imperatively through Emulator::surface().
 */
class Emulator extends Element
{
    protected string $type = 'emulator';

    /** @var array{name: string, z_index: int, input_capture: string} */
    protected array $emulatorProps = [
        'name' => 'main',
        'z_index' => 0,
        'input_capture' => InputCapture::Focus->value,
    ];

    private ?string $system = null;

    /** @var array<string, mixed>|null */
    private ?array $config = null;

    /** @var array<string, mixed>|null */
    private ?array $systemConfig = null;

    private ?string $rom = null;

    public static function make(string $name = 'main'): static
    {
        $el = new static;
        $el->emulatorProps['name'] = $name;

        return $el;
    }

    public function name(string $name): static
    {
        $this->emulatorProps['name'] = $name;

        return $this;
    }

    /**
     * iOS maps to layer.zPosition; Android snaps to the nearest
     * SurfaceView Z flag (default / media-overlay / on-top).
     */
    public function zIndex(int $zIndex): static
    {
        $this->emulatorProps['z_index'] = $zIndex;

        return $this;
    }

    /**
     * Choose how the surface receives hardware gamepad input. Global grabs the
     * pad at the window so it always drives the game; Focus (default) only
     * while the view is focused, leaving the pad for the app's own UI.
     */
    public function inputCapture(InputCapture $mode): static
    {
        $this->emulatorProps['input_capture'] = $mode->value;

        return $this;
    }

    /** System to boot when the surface mounts. */
    public function system(System|string $system): static
    {
        $this->system = $system instanceof System ? $system->value : $system;

        return $this;
    }

    /** Cross-system defaults; a system config passed alongside overrides these. */
    public function config(Config $config): static
    {
        $this->config = $config->toArray();

        return $this;
    }

    public function systemConfig(SystemConfig $config): static
    {
        $this->systemConfig = $config->toArray();

        return $this;
    }

    /** ROM to load once the system boots. */
    public function rom(string $path): static
    {
        $this->rom = $path;

        return $this;
    }

    public function applyAttributes(array $attrs): void
    {
        if (isset($attrs['name'])) {
            $this->emulatorProps['name'] = (string) $attrs['name'];
        }
        if (isset($attrs['zIndex'])) {
            $this->emulatorProps['z_index'] = (int) $attrs['zIndex'];
        }
        if (isset($attrs['z-index'])) {
            $this->emulatorProps['z_index'] = (int) $attrs['z-index'];
        }
        $capture = $attrs['inputCapture'] ?? $attrs['input-capture'] ?? null;
        if ($capture !== null && InputCapture::tryFrom((string) $capture) !== null) {
            $this->emulatorProps['input_capture'] = (string) $capture;
        }
        $system = $attrs['system'] ?? null;
        if ($system !== null) {
            $this->system = $system instanceof System ? $system->value : (string) $system;
        }
        if (($config = $attrs['config'] ?? null) instanceof Config) {
            $this->config = $config->toArray();
        }
        $systemConfig = $attrs['system-config'] ?? $attrs['systemConfig'] ?? null;
        if ($systemConfig instanceof SystemConfig) {
            $this->systemConfig = $systemConfig->toArray();
        }
        if (isset($attrs['rom'])) {
            $this->rom = (string) $attrs['rom'];
        }
    }

    protected function resolveProps(CallbackRegistry $registry): array
    {
        $props = $this->emulatorProps;

        if ($this->system !== null) {
            $props['system'] = $this->system;
        }

        // Global config ⊕ system config (system wins), so native applies one
        // effective config on mount.
        $config = array_merge($this->config ?? [], $this->systemConfig ?? []);

        // inputCapture is resolved at surface creation, not pushed to the core,
        // so hoist it onto the surface prop instead of the config map.
        if (isset($config['inputCapture'])) {
            $props['input_capture'] = $config['inputCapture'];
            unset($config['inputCapture']);
        }

        // EDGE props are scalar-only (no nested map), so config crosses as JSON.
        if ($config !== []) {
            $props['config'] = json_encode($config);
        }
        if ($this->rom !== null) {
            $props['rom'] = $this->rom;
        }

        return $props;
    }
}
