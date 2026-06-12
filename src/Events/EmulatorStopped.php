<?php

namespace KevinBatdorf\RetroEmulator\Events;

use Illuminate\Queue\SerializesModels;

class EmulatorStopped
{
    use SerializesModels;

    public function __construct(
        public string $surface,
    ) {}
}
