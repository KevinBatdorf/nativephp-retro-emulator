<?php

namespace KevinBatdorf\RetroEmulator\Elements;

use Native\Mobile\Edge\CallbackRegistry;
use Native\Mobile\Edge\Element;

/**
 * EDGE element behind `<native:dpad />` — an on-screen directional pad for the
 * emulator surface named by `surface`.
 *
 * It is one input area, not four buttons: native resolves the finger's position
 * into the 1–2 directions a physical pad would report, so diagonals work and a
 * thumb sliding off the pad keeps walking. Presses go straight into the core on
 * the native side, so no PHP runs per press and nothing re-renders.
 */
class Dpad extends Element
{
    protected string $type = 'dpad';

    /** @var array<string, mixed> */
    protected array $dpadProps = [
        'surface' => 'main',
        'port' => 1,
    ];

    private ?float $threshold = null;

    private ?float $diagonalRatio = null;

    private ?string $color = null;

    private ?string $activeColor = null;

    public static function make(string $surface = 'main'): static
    {
        $el = new static;
        $el->dpadProps['surface'] = $surface;

        return $el;
    }

    /** Emulator surface this pad drives — matches `<native:emulator name="…">`. */
    public function surface(string $surface): static
    {
        $this->dpadProps['surface'] = $surface;

        return $this;
    }

    /** Controller port to drive; a multitap's players are ports 2..5. */
    public function port(int $port): static
    {
        $this->dpadProps['port'] = $port;

        return $this;
    }

    /**
     * How far off centre an axis travels before its direction engages, as a
     * fraction of the pad's half-extent. Also the dead zone: a finger inside the
     * centre square reads neutral. Lower engages sooner.
     */
    public function threshold(float $threshold): static
    {
        $this->threshold = $threshold;

        return $this;
    }

    /**
     * How hard the weaker axis must compete before a diagonal forms, as a
     * fraction of the stronger axis. 0 gives free diagonals; raising it keeps
     * cardinals clean when a thumb drifts.
     */
    public function diagonalRatio(float $ratio): static
    {
        $this->diagonalRatio = $ratio;

        return $this;
    }

    public function color(string $color): static
    {
        $this->color = $color;

        return $this;
    }

    /** Fill for a direction while it is held. */
    public function activeColor(string $color): static
    {
        $this->activeColor = $color;

        return $this;
    }

    public function applyAttributes(array $attrs): void
    {
        if (isset($attrs['surface'])) {
            $this->dpadProps['surface'] = (string) $attrs['surface'];
        }
        if (isset($attrs['port'])) {
            $this->dpadProps['port'] = (int) $attrs['port'];
        }
        if (isset($attrs['threshold'])) {
            $this->threshold = (float) $attrs['threshold'];
        }
        $ratio = $attrs['diagonalRatio'] ?? $attrs['diagonal-ratio'] ?? null;
        if ($ratio !== null) {
            $this->diagonalRatio = (float) $ratio;
        }
        if (isset($attrs['color'])) {
            $this->color = (string) $attrs['color'];
        }
        $activeColor = $attrs['activeColor'] ?? $attrs['active-color'] ?? null;
        if ($activeColor !== null) {
            $this->activeColor = (string) $activeColor;
        }
    }

    protected function resolveProps(CallbackRegistry $registry): array
    {
        $props = $this->dpadProps;

        // Omitted props stay absent so each renderer applies its own default,
        // keeping the two platforms' feel defined in one place per platform.
        if ($this->threshold !== null) {
            $props['threshold'] = $this->threshold;
        }
        if ($this->diagonalRatio !== null) {
            $props['diagonal_ratio'] = $this->diagonalRatio;
        }
        if ($this->color !== null) {
            $props['color'] = $this->color;
        }
        if ($this->activeColor !== null) {
            $props['active_color'] = $this->activeColor;
        }

        return $props;
    }
}
