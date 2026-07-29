<?php

namespace KevinBatdorf\RetroEmulator\Config;

use KevinBatdorf\RetroEmulator\AspectCorrection;
use KevinBatdorf\RetroEmulator\InputCapture;
use KevinBatdorf\RetroEmulator\VideoOutput;

/**
 * A single system's config: everything shared from Config, plus the one key
 * that is never shareable — the path to that system's BIOS/firmware. Subclassed
 * per system because each system's remaining options are genuinely unique.
 */
abstract class SystemConfig extends Config
{
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
        public ?string $biosPath = null,
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
        );
    }

    /** @return array<string, mixed> */
    public function toArray(): array
    {
        return parent::toArray() + array_filter([
            'biosPath' => $this->biosPath,
        ], fn ($value) => $value !== null);
    }
}
