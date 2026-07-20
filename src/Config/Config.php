<?php

namespace KevinBatdorf\RetroEmulator\Config;

use KevinBatdorf\RetroEmulator\AspectCorrection;
use KevinBatdorf\RetroEmulator\InputCapture;
use KevinBatdorf\RetroEmulator\VideoOutput;

/**
 * Cross-system configuration — the knobs that mean the same thing on every
 * emulated system, so an app can define one house style and reuse it. A
 * per-system config (SfcConfig, GbConfig, …) extends this to override any of
 * these and add its own system-specific keys. All fields are nullable; only
 * the ones you set are sent, so the native defaults stay authoritative.
 */
class Config
{
    /**
     * @param  bool|null  $overscan  Trims the overscan border (trims by default);
     *                               no effect on systems with no overscan region.
     * @param  bool|null  $colorBleed  Composite color-bleed filter; no effect on
     *                                 systems without composite video (handhelds).
     * @param  InputCapture|null  $inputCapture  Resolved when the surface is
     *                                           created; not changeable at runtime.
     * @param  int|null  $runAhead  0 or 1 — ares runs exactly one hidden frame.
     * @param  string|null  $shader  librashader .slangp path; clear an active
     *                               shader with setShader(null), not null here.
     */
    public function __construct(
        public ?int $luminance = null,
        public ?int $saturation = null,
        public ?float $gamma = null,
        public ?bool $colorBleed = null,
        public ?bool $overscan = null,
        public ?VideoOutput $output = null,
        public ?int $fixedScale = null,
        public ?AspectCorrection $aspectCorrection = null,
        public ?int $volume = null,
        public ?int $balance = null,
        public ?InputCapture $inputCapture = null,
        public ?bool $autoSave = null,
        public ?float $speed = null,
        public ?int $runAhead = null,
        public ?bool $rewind = null,
        public ?int $rewindBufferSeconds = null,
        public ?bool $dynamicRateControl = null,
        public ?bool $rumble = null,
        public ?string $shader = null,
    ) {}

    /** @return array<string, mixed> */
    public function toArray(): array
    {
        return array_filter([
            'luminance' => $this->luminance,
            'saturation' => $this->saturation,
            'gamma' => $this->gamma,
            'colorBleed' => $this->colorBleed,
            'overscan' => $this->overscan,
            'output' => $this->output?->value,
            'fixedScale' => $this->fixedScale,
            'aspectCorrection' => $this->aspectCorrection?->value,
            'volume' => $this->volume,
            'balance' => $this->balance,
            'inputCapture' => $this->inputCapture?->value,
            'autoSave' => $this->autoSave,
            'speed' => $this->speed,
            'runAhead' => $this->runAhead,
            'rewind' => $this->rewind,
            'rewindBufferSeconds' => $this->rewindBufferSeconds,
            'dynamicRateControl' => $this->dynamicRateControl,
            'rumble' => $this->rumble,
            'shader' => $this->shader,
        ], fn ($value) => $value !== null);
    }
}
