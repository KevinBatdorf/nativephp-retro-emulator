<?php

namespace KevinBatdorf\RetroEmulator\Events;

use Illuminate\Foundation\Events\Dispatchable;
use Illuminate\Queue\SerializesModels;

class MemoryChanged
{
    use Dispatchable, SerializesModels;

    public function __construct(
        public string $surface,
        public int $address,
        public int $oldValue,
        public int $newValue,
    ) {}
}
