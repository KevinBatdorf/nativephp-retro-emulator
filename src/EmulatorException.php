<?php

namespace KevinBatdorf\RetroEmulator;

/**
 * Thrown synchronously for programmer errors — a call the dev got wrong
 * (unsupported system, no core loaded, a bad address, an unknown button).
 * Operational outcomes (a missing ROM, a failed save) never throw; they
 * arrive as an EmulatorError event instead.
 */
class EmulatorException extends \RuntimeException
{
    public function __construct(
        public readonly EmulatorErrorCode $errorCode,
        string $message,
    ) {
        parent::__construct($message);
    }
}
