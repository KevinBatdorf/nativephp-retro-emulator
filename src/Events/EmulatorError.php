<?php

namespace KevinBatdorf\RetroEmulator\Events;

use Illuminate\Queue\SerializesModels;
use KevinBatdorf\RetroEmulator\EmulatorErrorCode;

class EmulatorError
{
    use SerializesModels;

    public readonly EmulatorErrorCode $code;

    /**
     * Native dispatches this event via `new EmulatorError(...$payload)`, so
     * $code arrives as the raw wire string and is coerced to the enum here —
     * a promoted enum param can't accept the string spread from the payload.
     */
    public function __construct(
        public readonly string $surface,
        string $code,
        public readonly string $message,
    ) {
        $this->code = EmulatorErrorCode::from($code);
    }
}
