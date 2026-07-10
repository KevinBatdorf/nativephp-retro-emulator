<?php

namespace KevinBatdorf\RetroEmulator\Components;

use Illuminate\View\Component;
use Native\Mobile\Edge\NativeElementCollector;

/**
 * `<native-emulator />` component-tag form. Emits an `emulator` EDGE element
 * instead of HTML; the precompiled `<native:emulator />` form bypasses this
 * class entirely.
 */
class Emulator extends Component
{
    public function __construct(
        public string $name = 'main',
        public int $zIndex = 0,
    ) {}

    public function render(): callable
    {
        return function (): string {
            NativeElementCollector::leaf('emulator', [
                'name' => $this->name,
                'zIndex' => $this->zIndex,
            ]);

            return '';
        };
    }
}
