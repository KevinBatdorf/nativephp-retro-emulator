<?php

namespace KevinBatdorf\RetroEmulator\Config;

use KevinBatdorf\RetroEmulator\Accuracy;
use KevinBatdorf\RetroEmulator\AspectCorrection;
use KevinBatdorf\RetroEmulator\Backend;
use KevinBatdorf\RetroEmulator\InputCapture;
use KevinBatdorf\RetroEmulator\VideoOutput;

/**
 * A single system's config: everything shared from Config, plus the keys that
 * are never shareable — the system's BIOS/firmware path and the engine that
 * should serve it. Subclassed per system because each system's remaining
 * options are genuinely unique.
 */
abstract class SystemConfig extends Config
{
    /**
     * Engine-declared options (libretro core options), key => value string,
     * e.g. ['snes9x_overclock' => '150%']. Validated when the system loads
     * against the schema the core itself declares — an undeclared key or
     * value throws UNSUPPORTED_OPTION naming the legal set; it never
     * silently no-ops. What a declared option DOES is the core author's
     * contract, not this plugin's — use at your own risk. Bundled engines
     * (ares, SameBoy, mGBA) declare none; their settings are the typed keys
     * on this config.
     *
     * @var array<string, string>
     */
    public array $engineOptions = [];

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
        public ?string $biosPath = null,
        public Backend|string|null $backend = null,
        array $engineOptions = [],
    ) {
        foreach ($engineOptions as $key => $value) {
            if (! is_string($key) || ! (is_string($value) || is_int($value) || is_float($value))) {
                throw new \InvalidArgumentException(
                    "engineOptions must map option keys to string values — ['{$key}' => …] is not; "
                    ."pass the exact value string the core declares (e.g. 'enabled')"
                );
            }
            $this->engineOptions[$key] = (string) $value;
        }
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
        );
    }

    /** @return array<string, mixed> */
    public function toArray(): array
    {
        return parent::toArray() + array_filter([
            'biosPath' => $this->biosPath,
            'backend' => $this->backend instanceof Backend
                ? $this->backend->value
                : $this->backend,
            'engineOptions' => $this->engineOptions !== [] ? $this->engineOptions : null,
        ], fn ($value) => $value !== null);
    }
}
