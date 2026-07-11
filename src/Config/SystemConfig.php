<?php

namespace KevinBatdorf\RetroEmulator\Config;

/**
 * System-level config passed to loadSystem(). Only explicitly-set options are
 * sent over the bridge, so native defaults stay authoritative. Subclassed per
 * system because each system's options are genuinely unique.
 */
abstract class SystemConfig
{
    public function __construct(
        public ?string $biosPath = null,
        public ?bool $autoSave = null,
        public ?float $speed = null,
        public ?int $runAhead = null,
        public ?bool $rewind = null,
        public ?int $rewindBufferSeconds = null,
    ) {}

    /** @return array<string, mixed> */
    public function toArray(): array
    {
        return array_filter([
            'biosPath' => $this->biosPath,
            'autoSave' => $this->autoSave,
            'speed' => $this->speed,
            'runAhead' => $this->runAhead,
            'rewind' => $this->rewind,
            'rewindBufferSeconds' => $this->rewindBufferSeconds,
        ], fn ($value) => $value !== null);
    }
}
