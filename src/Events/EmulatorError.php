<?php

namespace KevinBatdorf\RetroEmulator\Events;

use Illuminate\Queue\SerializesModels;

class EmulatorError
{
    use SerializesModels;

    public function __construct(
        public string $surface,
        public string $code,
        public string $message,
    ) {}
}
