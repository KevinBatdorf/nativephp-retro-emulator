<?php

namespace KevinBatdorf\RetroEmulator\Components;

use Illuminate\View\Component;
use Native\Mobile\Edge\NativeElementCollector;

/**
 * `<native-dpad />` component-tag form. Emits a `dpad` EDGE element instead of
 * HTML; the precompiled `<native:dpad />` form bypasses this class entirely.
 * Forwards the same props as the Element so both tag forms are interchangeable.
 */
class Dpad extends Component
{
    public function __construct(
        public string $surface = 'main',
        public int $port = 1,
        public ?float $threshold = null,
        public ?float $diagonalRatio = null,
        public ?string $color = null,
        public ?string $activeColor = null,
    ) {}

    public function render(): callable
    {
        return function (): string {
            $attrs = [
                'surface' => $this->surface,
                'port' => $this->port,
            ];

            if ($this->threshold !== null) {
                $attrs['threshold'] = $this->threshold;
            }
            if ($this->diagonalRatio !== null) {
                $attrs['diagonalRatio'] = $this->diagonalRatio;
            }
            if ($this->color !== null) {
                $attrs['color'] = $this->color;
            }
            if ($this->activeColor !== null) {
                $attrs['activeColor'] = $this->activeColor;
            }

            NativeElementCollector::leaf('dpad', $attrs);

            return '';
        };
    }
}
