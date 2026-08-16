<?php

namespace KevinBatdorf\RetroEmulator\Events;

use Illuminate\Foundation\Events\Dispatchable;
use Illuminate\Queue\SerializesModels;

/**
 * The document picker closed: `path` is the copied file's absolute path, or
 * '' when the user cancelled. A pick failing validation dispatches
 * EmulatorError (INVALID_ROM) instead.
 */
class RomPicked
{
    use Dispatchable, SerializesModels;

    public function __construct(
        public string $surface,
        public string $path,
    ) {}
}
