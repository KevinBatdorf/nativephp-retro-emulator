<?php

namespace KevinBatdorf\RetroEmulator\Events;

use Illuminate\Foundation\Events\Dispatchable;
use Illuminate\Queue\SerializesModels;

class MemoryRead
{
    use Dispatchable, SerializesModels;

    /**
     * @param  int[]  $bytes
     */
    public function __construct(
        public string $surface,
        public int $address,
        public array $bytes,
    ) {}
}
