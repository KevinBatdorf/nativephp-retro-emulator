<?php

namespace KevinBatdorf\RetroEmulator\Config;

use KevinBatdorf\RetroEmulator\AspectCorrection;
use KevinBatdorf\RetroEmulator\InputCapture;
use KevinBatdorf\RetroEmulator\Region;
use KevinBatdorf\RetroEmulator\VideoOutput;

class Ps1Config extends RegionalSystemConfig
{
    /**
     * fastBoot skips the BIOS boot animation. The PS1 always needs firmware —
     * pass the BIOS dump via biosPath.
     */
    public function __construct(
        ?int $luminance = null,
        ?int $saturation = null,
        ?float $gamma = null,
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
        ?Region $region = null,
        ?array $preferredRegions = null,
        public ?bool $fastBoot = null,
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
            region: $region,
            preferredRegions: $preferredRegions,
        );
    }

    /** @return array<string, mixed> */
    public function toArray(): array
    {
        return parent::toArray() + array_filter([
            'fastBoot' => $this->fastBoot,
        ], fn ($value) => $value !== null);
    }
}
