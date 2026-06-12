<?php

namespace KevinBatdorf\RetroEmulator\Components;

use Illuminate\View\Component;
use Illuminate\View\View;

class Emulator extends Component
{
    public function __construct(
        public string $name = 'main',
        public int $zIndex = 0,
    ) {}

    public function render(): View
    {
        return view('retro-emulator::emulator');
    }
}
