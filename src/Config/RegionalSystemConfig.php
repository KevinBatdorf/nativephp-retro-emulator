<?php

namespace KevinBatdorf\RetroEmulator\Config;

use KevinBatdorf\RetroEmulator\Region;

/**
 * Config base for systems with region variants — Game Boy is region-free and
 * stays on SystemConfig. Region resolves automatically from the ROM analysis;
 * these knobs are the overrides.
 */
abstract class RegionalSystemConfig extends SystemConfig
{
    /** @param Region[]|null $preferredRegions Preference order for multi-region ROMs; default matches desktop-ares ("NTSC-U"). */
    public function __construct(
        ?string $biosPath = null,
        ?bool $autoSave = null,
        ?float $speed = null,
        ?int $runAhead = null,
        ?bool $rewind = null,
        ?int $rewindBufferSeconds = null,
        ?bool $dynamicRateControl = null,
        public ?Region $region = null,
        public ?array $preferredRegions = null,
    ) {
        parent::__construct(
            $biosPath, $autoSave, $speed, $runAhead,
            $rewind, $rewindBufferSeconds, $dynamicRateControl,
        );
    }

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
