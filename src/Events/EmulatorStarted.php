<?php

namespace KevinBatdorf\RetroEmulator\Events;

use Illuminate\Queue\SerializesModels;

class EmulatorStarted
{
    use SerializesModels;

    public function __construct(
        public string $surface,
        public string $system,
        public string $romPath,
    ) {}
}
