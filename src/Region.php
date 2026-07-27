<?php

namespace KevinBatdorf\RetroEmulator;

/**
 * Console region variants as ares' wire strings. Region resolves from ROM
 * analysis at load; these feed the `region:` / `preferredRegions:` overrides.
 * An override the loaded system doesn't support fails the load with a clean error.
 */
enum Region: string
{
    case NtscU = 'NTSC-U';
    case NtscJ = 'NTSC-J';
    case Ntsc = 'NTSC';
    case Pal = 'PAL';
    case Secam = 'SECAM';
}
