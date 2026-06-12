<?php

namespace KevinBatdorf\RetroEmulator\Events;

use Illuminate\Queue\SerializesModels;

class MemoryRead
{
    use SerializesModels;

    /**
     * @param  int[]  $bytes
     */
    public function __construct(
        public string $surface,
        public int $address,
        public array $bytes,
    ) {}
}
