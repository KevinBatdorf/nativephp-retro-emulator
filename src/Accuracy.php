<?php

namespace KevinBatdorf\RetroEmulator;

/**
 * Renderer preset, applied at boot (LoadSystem) — it picks the renderer
 * implementation, so changing it means rebooting the system; a post-boot
 * configure() rejects it.
 *
 * Performance renders per scanline. Accurate renders per dot/cycle where a
 * core offers the choice (SNES swaps in its dot-accurate PPU, GBA adds real
 * VRAM/palette bus contention) and costs measurably more CPU; cores with a
 * single renderer (NES, GB/GBC, Mega Drive) accept either value and ignore it.
 */
enum Accuracy: string
{
    /** Scanline renderers — the default, and what phones can afford. */
    case Performance = 'performance';

    /** Dot/cycle renderers where available — mid-scanline effects render right. */
    case Accurate = 'accurate';
}
