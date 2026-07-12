<?php

namespace KevinBatdorf\RetroEmulator\Config;

use KevinBatdorf\RetroEmulator\Region;

class SfcConfig extends RegionalSystemConfig
{
    /**
     * deepBlackBoost applies a gamma ramp that crushes black levels. Defaults
     * off; ares' sfc core defaults it on, so leaving it unset actively disables
     * it (deliberate — see plan.md item 6).
     */
    public function __construct(
        ?string $biosPath = null,
        ?bool $autoSave = null,
        ?float $speed = null,
        ?int $runAhead = null,
        ?bool $rewind = null,
        ?int $rewindBufferSeconds = null,
        ?bool $dynamicRateControl = null,
        ?Region $region = null,
        ?array $preferredRegions = null,
        public ?bool $deepBlackBoost = null,
    ) {
        parent::__construct(
            $biosPath, $autoSave, $speed, $runAhead, $rewind,
            $rewindBufferSeconds, $dynamicRateControl, $region, $preferredRegions,
        );
    }

    public function toArray(): array
    {
        return parent::toArray() + array_filter([
            'deepBlackBoost' => $this->deepBlackBoost,
        ], fn ($value) => $value !== null);
    }
}
