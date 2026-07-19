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
    case N64 = 'n64';
    case Gb = 'gb';
    case Gbc = 'gbc';
    case Gba = 'gba';
    case Sg = 'sg';
    case Ms = 'ms';
    case Md = 'md';
    case Pce = 'pce';
    case Ngp = 'ngp';
    case Ws = 'ws';
    case Wsc = 'wsc';
    case Ps1 = 'ps1';
    case Ng = 'ng';
    case A26 = 'a26';
    case Msx = 'msx';
}
