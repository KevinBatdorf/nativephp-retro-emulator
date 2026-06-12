<?php

namespace KevinBatdorf\RetroEmulator\Events;

use Illuminate\Queue\SerializesModels;

class MemoryChanged
{
    use SerializesModels;

    public function __construct(
        public string $surface,
        public int $address,
        public int $oldValue,
        public int $newValue,
    ) {}
}
