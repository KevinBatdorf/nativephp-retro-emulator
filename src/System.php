<?php

namespace KevinBatdorf\RetroEmulator;

/**
 * ares system ids as reported by Emulator::systems(). Whether a system is
 * compiled into the current native build is runtime knowledge — check the
 * `supported` flag on systems() before loading.
 */
enum System: string
{
    case Fc = 'fc';
    case Sfc = 'sfc';
    case Gb = 'gb';
    case Gbc = 'gbc';
    case Gba = 'gba';
    case Md = 'md';
}
