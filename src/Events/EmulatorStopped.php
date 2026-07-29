<?php

namespace KevinBatdorf\RetroEmulator\Events;

use Illuminate\Foundation\Events\Dispatchable;
use Illuminate\Queue\SerializesModels;

class EmulatorStopped
{
    use Dispatchable, SerializesModels;

    public function __construct(
        public string $surface,
    ) {}
}
