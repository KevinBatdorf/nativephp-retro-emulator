<?php

namespace KevinBatdorf\RetroEmulator;

use KevinBatdorf\RetroEmulator\Concerns\InteractsWithBridge;

/**
 * A controller connected to one port of a surface — the handle returned by
 * {@see Emulator::connectDevice()} / {@see Emulator::getDevice()}. All runtime
 * input lives here: press/release buttons, set axes (mouse / light-gun motion),
 * and remap. State is cumulative and native, so independent overlay buttons
 * compose (hold Up, then B; releasing Up leaves B held).
 *
 * Names — buttons and axes — are the ones {@see Emulator::ports()} reports for
 * the connected device. An unknown name is a programmer error (throws).
 */
class Controller
{
    use InteractsWithBridge;

    public function __construct(
        private readonly string $surface,
        public readonly int $port,
    ) {}

    /** Hold a button down. Additive — other held buttons are untouched. */
    public function press(\BackedEnum|string $button): static
    {
        $this->call('Emulator.PressButton', [
            'surface' => $this->surface,
            'port' => $this->port,
            'button' => $button instanceof \BackedEnum ? $button->value : $button,
        ]);

        return $this;
    }

    /** Release a single button; other held buttons stay held. */
    public function release(\BackedEnum|string $button): static
    {
        $this->call('Emulator.ReleaseButton', [
            'surface' => $this->surface,
            'port' => $this->port,
            'button' => $button instanceof \BackedEnum ? $button->value : $button,
        ]);

        return $this;
    }

    /**
     * Set several buttons at once as a snapshot — use when you compute the whole
     * state (e.g. a virtual thumbstick), not for independent overlay buttons.
     *
     * @param  array<string, bool>  $state  button name => held
     */
    public function setButtons(array $state): static
    {
        $this->call('Emulator.SetButtons', [
            'surface' => $this->surface,
            'port' => $this->port,
            'state' => $state,
        ]);

        return $this;
    }

    /**
     * Feed a relative motion delta on an axis (mouse / light-gun X/Y). The
     * hardware is relative: light-guns accumulate deltas into an internal
     * cursor. Consumed once per emulated poll.
     */
    public function setAxis(string $axis, int $value): static
    {
        $this->call('Emulator.SetAxis', [
            'surface' => $this->surface,
            'port' => $this->port,
            'axis' => $axis,
            'value' => $value,
        ]);

        return $this;
    }

    /**
     * Aim a light-gun (Super Scope / Justifier) at an absolute position on the
     * emulated screen, given as normalized 0..1 coordinates (0,0 = top-left,
     * 1,1 = bottom-right). The hardware is relative-only, so the plugin tracks a
     * shadow cursor and feeds the delta to reach the target. For light-gun
     * devices; a device without X/Y axes throws.
     */
    public function aimAt(float $x, float $y): static
    {
        $this->call('Emulator.AimAt', [
            'surface' => $this->surface,
            'port' => $this->port,
            'x' => $x,
            'y' => $y,
        ]);

        return $this;
    }

    /**
     * Merge a button remap for this controller: each `emulated => source` pair
     * points an in-game button at a different input (`['a' => 'b', 'b' => 'a']`
     * swaps A and B). Composes on the device defaults; an empty array resets.
     *
     * @param  array<string, string>  $mappings  emulated button => source input
     */
    public function remap(array $mappings): static
    {
        $this->call('Emulator.SetInputMapping', [
            'surface' => $this->surface,
            'port' => $this->port,
            'mappings' => $mappings,
        ]);

        return $this;
    }
}
