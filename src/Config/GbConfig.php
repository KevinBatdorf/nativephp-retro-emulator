<?php

namespace KevinBatdorf\RetroEmulator\Config;

class GbConfig extends SystemConfig
{
    /**
     * colorEmulation matches CGB colors to a real Game Boy Color screen (no
     * effect on original Game Boy — that's a separate palette setting).
     * interframeBlending emulates handheld-LCD ghosting so flicker-transparency
     * effects read as translucent. Both default off; enable to opt in.
     */
    public function __construct(
        ?string $biosPath = null,
        ?bool $autoSave = null,
        ?float $speed = null,
        ?int $runAhead = null,
        ?bool $rewind = null,
        ?int $rewindBufferSeconds = null,
        ?bool $dynamicRateControl = null,
        public ?bool $colorEmulation = null,
        public ?bool $interframeBlending = null,
    ) {
        parent::__construct(
            $biosPath, $autoSave, $speed, $runAhead,
            $rewind, $rewindBufferSeconds, $dynamicRateControl,
        );
    }

    public function toArray(): array
    {
        return parent::toArray() + array_filter([
            'colorEmulation' => $this->colorEmulation,
            'interframeBlending' => $this->interframeBlending,
        ], fn ($value) => $value !== null);
    }
}
