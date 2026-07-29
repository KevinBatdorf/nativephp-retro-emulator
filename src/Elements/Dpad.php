<?php

namespace KevinBatdorf\RetroEmulator\Elements;

use InvalidArgumentException;
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
 *
 * Nothing here is mandatory — every prop has a default, so `<native:dpad />`
 * alone gives a working pad. Size comes from the usual layout classes
 * (`class="w-36 h-36"`); the props below cover how it looks and feels. Each is a
 * whole PERCENTAGE, matching the picture and audio options, and an out-of-range
 * value throws rather than silently producing a dead pad.
 */
class Dpad extends Element
{
    /** Prop name => [min, max] percent, all inclusive. */
    private const PERCENT_RANGES = [
        'threshold' => [5, 90],
        'diagonalRatio' => [0, 95],
        'thickness' => [10, 60],
        'radius' => [0, 50],
    ];

    protected string $type = 'dpad';

    /** @var array<string, mixed> */
    protected array $dpadProps = [
        'surface' => 'main',
        'port' => 1,
    ];

    private ?int $threshold = null;

    private ?int $diagonalRatio = null;

    private ?int $thickness = null;

    private ?int $radius = null;

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
     * percent of the pad's half-extent. Doubles as the dead zone: a finger inside
     * the centre square reads neutral. Lower engages sooner. Default 33.
     */
    public function threshold(int $percent): static
    {
        $this->threshold = $this->percent('threshold', $percent);

        return $this;
    }

    /**
     * How hard the weaker axis must compete before a diagonal forms, as a percent
     * of the stronger axis. 0 (default) gives free diagonals; raising it keeps
     * cardinals clean under a drifting thumb, at the cost of the second direction
     * arriving later.
     */
    public function diagonalRatio(int $percent): static
    {
        $this->diagonalRatio = $this->percent('diagonalRatio', $percent);

        return $this;
    }

    /** Arm width as a percent of the pad's shorter side. Default 36. */
    public function thickness(int $percent): static
    {
        $this->thickness = $this->percent('thickness', $percent);

        return $this;
    }

    /**
     * Corner rounding as a percent of the arm's width: 0 is square, 50 gives a
     * fully rounded tip. Default 28.
     */
    public function radius(int $percent): static
    {
        $this->radius = $this->percent('radius', $percent);

        return $this;
    }

    /** Resting fill, as any color the layout classes accept (`#RRGGBB`, `#AARRGGBB`). */
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
            $this->threshold($this->intAttr('threshold', $attrs['threshold']));
        }
        $ratio = $attrs['diagonalRatio'] ?? $attrs['diagonal-ratio'] ?? null;
        if ($ratio !== null) {
            $this->diagonalRatio($this->intAttr('diagonalRatio', $ratio));
        }
        if (isset($attrs['thickness'])) {
            $this->thickness($this->intAttr('thickness', $attrs['thickness']));
        }
        if (isset($attrs['radius'])) {
            $this->radius($this->intAttr('radius', $attrs['radius']));
        }
        if (isset($attrs['color'])) {
            $this->color = (string) $attrs['color'];
        }
        $activeColor = $attrs['activeColor'] ?? $attrs['active-color'] ?? null;
        if ($activeColor !== null) {
            $this->activeColor = (string) $activeColor;
        }
    }

    /**
     * A Blade attribute arrives as a string. Reject a fractional one before it
     * truncates: `threshold="0.33"` would become 0 and silently disable the pad.
     */
    private function intAttr(string $name, mixed $value): int
    {
        if (! is_numeric($value) || (float) $value !== (float) (int) $value) {
            throw new InvalidArgumentException(
                "dpad {$name} is a whole percentage, got '{$value}'",
            );
        }

        return (int) $value;
    }

    private function percent(string $name, int $value): int
    {
        [$min, $max] = self::PERCENT_RANGES[$name];

        if ($value < $min || $value > $max) {
            throw new InvalidArgumentException(
                "dpad {$name} is a whole percentage ({$min}-{$max}), got {$value}",
            );
        }

        return $value;
    }

    protected function resolveProps(CallbackRegistry $registry): array
    {
        $props = $this->dpadProps;

        // Omitted props stay absent so each renderer applies its own default,
        // keeping the two platforms' feel defined in one place per platform.
        foreach (['threshold', 'diagonalRatio', 'thickness', 'radius'] as $name) {
            if ($this->{$name} !== null) {
                $props[strtolower(preg_replace('/(?<!^)[A-Z]/', '_$0', $name))] = $this->{$name};
            }
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
