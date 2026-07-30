<?php

namespace KevinBatdorf\RetroEmulator;

/**
 * Emulation engines a system can run on. Which engines actually serve a
 * system in the current build is runtime knowledge — Emulator::systems()
 * reports each system's `backends`. An unnamed boot runs the built-in
 * engine (ares); every other engine runs only when a config names it.
 * Pass a plain string instead of a case for bring-your-own cores.
 */
enum Backend: string
{
    case Ares = 'ares';
    case SameBoy = 'sameboy';
    case Mgba = 'mgba';
    case Libretro = 'libretro';
}
