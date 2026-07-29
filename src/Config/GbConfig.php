<?php

namespace KevinBatdorf\RetroEmulator\Config;

use KevinBatdorf\RetroEmulator\AspectCorrection;
use KevinBatdorf\RetroEmulator\InputCapture;
use KevinBatdorf\RetroEmulator\VideoOutput;

class GbConfig extends SystemConfig
{
    /**
     * colorEmulation matches CGB colors to a real Game Boy Color screen (no
     * effect on original Game Boy). interframeBlending emulates handheld-LCD
     * ghosting so flicker-transparency effects read as translucent. Both default
     * off; enable to opt in.
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
        ?string $biosPath = null,
        public ?bool $colorEmulation = null,
        public ?bool $interframeBlending = null,
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
            biosPath: $biosPath,
        );
    }

    /** @return array<string, mixed> */
    public function toArray(): array
    {
        return parent::toArray() + array_filter([
            'colorEmulation' => $this->colorEmulation,
            'interframeBlending' => $this->interframeBlending,
        ], fn ($value) => $value !== null);
    }
}
