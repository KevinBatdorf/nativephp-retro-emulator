<?php

namespace KevinBatdorf\RetroEmulator\Config;

use KevinBatdorf\RetroEmulator\Accuracy;
use KevinBatdorf\RetroEmulator\AspectCorrection;
use KevinBatdorf\RetroEmulator\Backend;
use KevinBatdorf\RetroEmulator\InputCapture;
use KevinBatdorf\RetroEmulator\Region;
use KevinBatdorf\RetroEmulator\VideoOutput;

/**
 * Config for systems with region variants — Game Boy is region-free and stays
 * on SystemConfig. Region resolves automatically from the ROM analysis; these
 * knobs are the overrides.
 */
abstract class RegionalSystemConfig extends SystemConfig
{
    /** @param  Region[]|null  $preferredRegions  Preference order for multi-region ROMs; defaults to desktop-ares ("NTSC-U"). */
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
        Backend|string|null $backend = null,
        public ?Region $region = null,
        public ?array $preferredRegions = null,
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
            backend: $backend,
        );
    }

    /** @return array<string, mixed> */
    public function toArray(): array
    {
        return parent::toArray() + array_filter([
            'region' => $this->region?->value,
            'preferredRegions' => $this->preferredRegions !== null
                ? array_map(fn (Region $region) => $region->value, $this->preferredRegions)
                : null,
        ], fn ($value) => $value !== null);
    }
}
