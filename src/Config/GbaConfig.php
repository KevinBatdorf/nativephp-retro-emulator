<?php

namespace KevinBatdorf\RetroEmulator\Config;

use KevinBatdorf\RetroEmulator\Accuracy;
use KevinBatdorf\RetroEmulator\AspectCorrection;
use KevinBatdorf\RetroEmulator\Backend;
use KevinBatdorf\RetroEmulator\InputCapture;
use KevinBatdorf\RetroEmulator\VideoOutput;

class GbaConfig extends SystemConfig
{
    /**
     * colorEmulation matches colors to a real GBA screen; interframeBlending
     * emulates handheld-LCD ghosting so flicker-transparency effects read as
     * translucent. Both are ares-only (capabilities()['toggles']); booting
     * mGBA with one enabled throws UNSUPPORTED_OPTION. Both default off.
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
        ?bool $rawAudio = null,
        ?bool $bootAnimation = null,
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
        array $engineOptions = [],
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
            rawAudio: $rawAudio,
            bootAnimation: $bootAnimation,
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
            engineOptions: $engineOptions,
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
