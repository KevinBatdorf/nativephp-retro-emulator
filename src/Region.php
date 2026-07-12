<?php

namespace KevinBatdorf\RetroEmulator;

/**
 * Console region variants, serialized to ares' wire strings. Region resolves
 * automatically from the ROM analysis at load time — these values feed the
 * `region:` override and `preferredRegions:` config knobs. SNES supports
 * Ntsc/Pal; NES and Mega Drive support NtscJ/NtscU/Pal; Game Boy is
 * region-free. An unsupported override fails the load with a clean error.
 */
enum Region: string
{
    case NtscU = 'NTSC-U';
    case NtscJ = 'NTSC-J';
    case Ntsc = 'NTSC';
    case Pal = 'PAL';
}
