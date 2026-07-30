<?php

namespace KevinBatdorf\RetroEmulator;

/**
 * Emulation engines a system can run on. Which engines actually serve a
 * system in the current build is runtime knowledge — Emulator::systems()
 * reports each system's `backends` and its `defaultBackend` (the bundled
 * fast core where one exists, ares otherwise). Pass a plain string instead
 * of a case for bring-your-own engines.
 */
enum Backend: string
{
    case Ares = 'ares';
    case SameBoy = 'sameboy';
    case Mgba = 'mgba';
    case Libretro = 'libretro';
}
