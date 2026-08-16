<?php

namespace KevinBatdorf\RetroEmulator\Events;

use Illuminate\Foundation\Events\Dispatchable;
use Illuminate\Queue\SerializesModels;

/**
 * Fires when rotation or a resize lands new window geometry. Values are dp;
 * insets are the system-obscured edges (bars, Dynamic Island, cutout).
 * Dispatched only while an emulator surface is mounted — the surface's own
 * resize is the hook.
 */
class WindowMetricsChanged
{
    use Dispatchable, SerializesModels;

    public function __construct(
        public readonly int $width,
        public readonly int $height,
        public readonly int $top,
        public readonly int $bottom,
        public readonly int $left,
        public readonly int $right,
    ) {}
}
