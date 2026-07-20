<?php

namespace KevinBatdorf\RetroEmulator\Config;

use KevinBatdorf\RetroEmulator\AspectCorrection;
use KevinBatdorf\RetroEmulator\ControllerPakBanks;
use KevinBatdorf\RetroEmulator\InputCapture;
use KevinBatdorf\RetroEmulator\N64Quality;
use KevinBatdorf\RetroEmulator\Region;
use KevinBatdorf\RetroEmulator\VideoOutput;

/**
 * The pre-load options desktop ares sets before every N64 boot
 * (nintendo-64.cpp:107-118). Unset fields keep desktop's defaults natively:
 * quality SD, supersampling off, VI processing on, weave deinterlacing on,
 * homebrew mode off, recompiler ON, Expansion Pak in, 32KiB Controller Pak.
 *
 * recompiler only has effect on Android — iOS compiles the CPU/RSP
 * interpreters (no JIT pages without an entitlement), so it no-ops there.
 */
class N64Config extends RegionalSystemConfig
{
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
        public ?N64Quality $quality = null,
        public ?bool $supersampling = null,
        public ?bool $disableVideoInterfaceProcessing = null,
        public ?bool $weaveDeinterlacing = null,
        public ?bool $homebrewMode = null,
        public ?bool $recompiler = null,
        public ?bool $expansionPak = null,
        public ?ControllerPakBanks $controllerPakBanks = null,
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
            'quality' => $this->quality?->value,
            'supersampling' => $this->supersampling,
            'disableVideoInterfaceProcessing' => $this->disableVideoInterfaceProcessing,
            'weaveDeinterlacing' => $this->weaveDeinterlacing,
            'homebrewMode' => $this->homebrewMode,
            'recompiler' => $this->recompiler,
            'expansionPak' => $this->expansionPak,
            'controllerPakBanks' => $this->controllerPakBanks?->value,
        ], fn ($value) => $value !== null);
    }
}
