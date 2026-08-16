<?php

namespace KevinBatdorf\RetroEmulator\Config;

use KevinBatdorf\RetroEmulator\Accuracy;
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
     * Percentage options are whole percents; 100 leaves the value untouched.
     * `speed` is a multiplier, not a percent.
     *
     * @param  int|null  $luminance  Percent, 0–100; 100 is untouched.
     * @param  int|null  $saturation  Percent, 0–100; 100 is untouched.
     * @param  int|null  $gamma  Percent, 100–200 like ares desktop; 100 is
     *                           untouched, higher darkens midtones.
     * @param  int|null  $volume  Percent, 0–100.
     * @param  int|null  $balance  Percent, −100 (full left) … +100 (full right).
     * @param  bool|null  $rawAudio  Restores each engine's own sound — today the
     *                               Game Boy DC handling in both engines plus
     *                               the 60 Hz filter. Loudness matching and
     *                               transition fades stay on: gain is
     *                               calibration, and app transitions have no
     *                               hardware behaviour to restore.
     * @param  bool|null  $bootAnimation  Plays the console's own boot animation
     *                                    (logo scroll, ding). Skipped by default:
     *                                    the boot runs hidden at full speed and
     *                                    the game starts where the console hands
     *                                    off. Systems without one are unaffected.
     * @param  float|null  $speed  Multiplier, 0.25–4.0; 1.0 is native speed.
     * @param  bool|null  $overscan  True shows the overscan border; the default
     *                               (false) trims it. No effect on systems with
     *                               no overscan region.
     * @param  bool|null  $colorBleed  Composite color-bleed filter; no effect on
     *                                 systems without composite video (handhelds).
     * @param  InputCapture|null  $inputCapture  Resolved when the surface is
     *                                           created; not changeable at runtime.
     * @param  int|null  $runAhead  0 or 1 — ares runs exactly one hidden frame.
     * @param  int|null  $rewindBufferSeconds  Seconds of history to retain.
     * @param  string|null  $shader  librashader .slangp path; clear an active
     *                               shader with setShader(null), not null here.
     * @param  Accuracy|null  $accuracy  Renderer preset — boot-only; changing it
     *                                   means rebooting the system.
     * @param  bool|null  $pixelAccuracy  Direct ares "Pixel Accuracy" override;
     *                                    beats $accuracy when both are set.
     */
    public function __construct(
        public ?int $luminance = null,
        public ?int $saturation = null,
        public ?int $gamma = null,
        public ?bool $colorBleed = null,
        public ?bool $overscan = null,
        public ?VideoOutput $output = null,
        public ?int $fixedScale = null,
        public ?AspectCorrection $aspectCorrection = null,
        public ?int $volume = null,
        public ?int $balance = null,
        public ?bool $rawAudio = null,
        public ?bool $bootAnimation = null,
        public ?InputCapture $inputCapture = null,
        public ?bool $autoSave = null,
        public ?float $speed = null,
        public ?int $runAhead = null,
        public ?bool $rewind = null,
        public ?int $rewindBufferSeconds = null,
        public ?bool $dynamicRateControl = null,
        public ?bool $rumble = null,
        public ?string $shader = null,
        public ?Accuracy $accuracy = null,
        public ?bool $pixelAccuracy = null,
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
            'rawAudio' => $this->rawAudio,
            'bootAnimation' => $this->bootAnimation,
            'inputCapture' => $this->inputCapture?->value,
            'autoSave' => $this->autoSave,
            'speed' => $this->speed,
            'runAhead' => $this->runAhead,
            'rewind' => $this->rewind,
            'rewindBufferSeconds' => $this->rewindBufferSeconds,
            'dynamicRateControl' => $this->dynamicRateControl,
            'rumble' => $this->rumble,
            'shader' => $this->shader,
            'pixelAccuracy' => $this->resolvedPixelAccuracy(),
        ], fn ($value) => $value !== null);
    }

    /**
     * The one wire value both knobs feed. Override beats preset; both unset
     * omits the key, leaving the native default (performance) authoritative.
     */
    private function resolvedPixelAccuracy(): ?bool
    {
        return $this->pixelAccuracy ?? match ($this->accuracy) {
            Accuracy::Accurate => true,
            Accuracy::Performance => false,
            null => null,
        };
    }
}
