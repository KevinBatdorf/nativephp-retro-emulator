<?php

namespace KevinBatdorf\RetroEmulator;

/**
 * Presentation size mode, mirroring ares desktop's Video output setting.
 * Modes that don't fit the surface fall back the way desktop does:
 * Integer → best-fit when even 1× overflows; IntegerFixed → largest
 * fitting multiple, then best-fit.
 */
enum VideoOutput: string
{
    /** Best-fit letterbox — largest size preserving aspect (default). */
    case Scale = 'scale';

    /** Largest whole multiple of the emulated resolution — crisp pixels. */
    case Integer = 'integer';

    /** Exactly fixedScale× the emulated resolution. */
    case IntegerFixed = 'integerFixed';

    /** Fill the surface, ignoring aspect. */
    case Stretch = 'stretch';
}
