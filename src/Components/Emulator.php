<?php

namespace KevinBatdorf\RetroEmulator\Components;

use Illuminate\View\Component;
use KevinBatdorf\RetroEmulator\Config\Config;
use KevinBatdorf\RetroEmulator\Config\SystemConfig;
use KevinBatdorf\RetroEmulator\InputCapture;
use KevinBatdorf\RetroEmulator\System;
use Native\Mobile\Edge\NativeElementCollector;

/**
 * `<native-emulator />` component-tag form. Emits an `emulator` EDGE element
 * instead of HTML; the precompiled `<native:emulator />` form bypasses this
 * class entirely. Forwards the same setup props as the Element so both tag
 * forms are interchangeable.
 */
class Emulator extends Component
{
    public function __construct(
        public string $name = 'main',
        public int $zIndex = 0,
        public ?InputCapture $inputCapture = null,
        public System|string|null $system = null,
        public ?Config $config = null,
        public ?SystemConfig $systemConfig = null,
        public ?string $rom = null,
    ) {}

    public function render(): callable
    {
        return function (): string {
            $attrs = [
                'name' => $this->name,
                'zIndex' => $this->zIndex,
            ];

            if ($this->inputCapture !== null) {
                $attrs['inputCapture'] = $this->inputCapture->value;
            }
            if ($this->system !== null) {
                $attrs['system'] = $this->system instanceof System ? $this->system->value : $this->system;
            }
            if ($this->config !== null) {
                $attrs['config'] = $this->config;
            }
            if ($this->systemConfig !== null) {
                $attrs['systemConfig'] = $this->systemConfig;
            }
            if ($this->rom !== null) {
                $attrs['rom'] = $this->rom;
            }

            NativeElementCollector::leaf('emulator', $attrs);

            return '';
        };
    }
}
