<?php

namespace KevinBatdorf\RetroEmulator\Elements;

class Emulator
{
    public function __construct(
        public string $name = 'main',
        public int $zIndex = 0,
    ) {}
}
