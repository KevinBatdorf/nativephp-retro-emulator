<?php

namespace KevinBatdorf\RetroEmulator;

/**
 * Pixel aspect handling, mirroring ares desktop's aspect-correction setting.
 */
enum AspectCorrection: string
{
    /** The system's true pixel aspect (e.g. SNES 8:7) — default. */
    case Standard = 'standard';

    /** Square pixels — no correction. */
    case None = 'none';

    /** Standard correction stretched a further 4:3. */
    case Anamorphic = 'anamorphic';
}
