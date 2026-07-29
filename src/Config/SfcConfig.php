<?php

namespace KevinBatdorf\RetroEmulator\Config;

use KevinBatdorf\RetroEmulator\Accuracy;
use KevinBatdorf\RetroEmulator\AspectCorrection;
use KevinBatdorf\RetroEmulator\InputCapture;
use KevinBatdorf\RetroEmulator\Region;
use KevinBatdorf\RetroEmulator\VideoOutput;

class SfcConfig extends RegionalSystemConfig
{
    /**
     * deepBlackBoost applies a gamma ramp that crushes black levels. Defaults
     * off; ares' sfc core defaults it on, so leaving it unset actively disables it.
     */
    public function __construct(
        ?int $luminance = null,
        ?int $saturation = null,
        ?int $gamma = null,
        ?bool $colorBleed = null,
        ?bool $overscan = null,
        ?VideoOutput $output = null,
        ?int $fixedScale = null,
        ?AspectCorrection $aspectCorrection = null,
        ?int $volume = null,
        ?int $balance = null,
        ?InputCapture $inputCapture = null,
        ?bool $autoSave = null,
        ?float $speed = null,
        ?int $runAhead = null,
        ?bool $rewind = null,
        ?int $rewindBufferSeconds = null,
        ?bool $dynamicRateControl = null,
        ?bool $rumble = null,
        ?string $shader = null,
        ?Accuracy $accuracy = null,
        ?bool $pixelAccuracy = null,
        ?string $biosPath = null,
        ?Region $region = null,
        ?array $preferredRegions = null,
        public ?bool $deepBlackBoost = null,
    ) {
        parent::__construct(
            luminance: $luminance,
            saturation: $saturation,
            gamma: $gamma,
            colorBleed: $colorBleed,
            overscan: $overscan,
            output: $output,
            fixedScale: $fixedScale,
            aspectCorrection: $aspectCorrection,
            volume: $volume,
            balance: $balance,
            inputCapture: $inputCapture,
            autoSave: $autoSave,
            speed: $speed,
            runAhead: $runAhead,
            rewind: $rewind,
            rewindBufferSeconds: $rewindBufferSeconds,
            dynamicRateControl: $dynamicRateControl,
            rumble: $rumble,
            shader: $shader,
            accuracy: $accuracy,
            pixelAccuracy: $pixelAccuracy,
            biosPath: $biosPath,
            region: $region,
            preferredRegions: $preferredRegions,
        );
    }

    /** @return array<string, mixed> */
    public function toArray(): array
    {
        return parent::toArray() + array_filter([
            'deepBlackBoost' => $this->deepBlackBoost,
        ], fn ($value) => $value !== null);
    }
}
