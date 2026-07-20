<?php

namespace KevinBatdorf\RetroEmulator;

/**
 * N64 render quality as ares' wire strings (internal parallel-RDP upscale:
 * SD = 1x native, HD = 2x, UHD = 4x). Higher tiers multiply GPU cost.
 */
enum N64Quality: string
{
    case Sd = 'SD';
    case Hd = 'HD';
    case Uhd = 'UHD';
}
