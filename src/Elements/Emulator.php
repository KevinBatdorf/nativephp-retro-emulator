<?php

namespace KevinBatdorf\RetroEmulator\Elements;

use Native\Mobile\Edge\CallbackRegistry;
use Native\Mobile\Edge\Element;

/**
 * EDGE element behind `<native:emulator />` — the serialized node is routed
 * to the native EmulatorRenderer (GLSurfaceView / MTKView) by type.
 */
class Emulator extends Element
{
    protected string $type = 'emulator';

    /** @var array{name: string, z_index: int} */
    protected array $emulatorProps = [
        'name' => 'main',
        'z_index' => 0,
    ];

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
    }

    protected function resolveProps(CallbackRegistry $registry): array
    {
        return $this->emulatorProps;
    }
}
