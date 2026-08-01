<?php

namespace KevinBatdorf\RetroEmulator\Elements;

use InvalidArgumentException;
use Native\Mobile\Edge\CallbackRegistry;
use Native\Mobile\Edge\Element;
use Native\Mobile\Edge\SharedValue;

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
 * (`class="w-36 h-36"`). Every feel prop is a whole PERCENTAGE, matching the
 * picture and audio options, and an out-of-range value throws rather than
 * silently producing a dead pad.
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

    private ?bool $diagonals = null;

    private ?string $changeMethod = null;

    private ?SharedValue $panX = null;

    private ?SharedValue $panY = null;

    private ?int $panSpeed = null;

    /** @var array{0: int, 1: int}|null */
    private ?array $panRangeX = null;

    /** @var array{0: int, 1: int}|null */
    private ?array $panRangeY = null;

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

    /**
     * Allow two directions at once. False locks the pad to four ways, snapping to
     * whichever axis the thumb is further along — `diagonalRatio` can only make a
     * diagonal unlikely, which is not enough for a game that has none.
     */
    public function diagonals(bool $allowed): static
    {
        $this->diagonals = $allowed;

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

    /**
     * Report the held directions on change, comma-separated ("Up,Right", or ""
     * on release). Bound from Blade as `@change="method"`. Only needed to drive
     * something other than the core; unbound, no press reaches PHP at all.
     */
    public function onChange(string $method): static
    {
        $this->changeMethod = $method;

        return $this;
    }

    /**
     * Integrate the held direction into a pair of SharedValues, in dp, on the
     * native frame clock. Bind them to a child's `:translate-x` / `:translate-y`
     * for motion at display rate; animating from `onChange` instead is capped by
     * how fast the host can republish its tree, which is far below a frame.
     */
    public function pan(?SharedValue $x = null, ?SharedValue $y = null): static
    {
        $this->panX = $x;
        $this->panY = $y;

        return $this;
    }

    /**
     * Bound `pan()` to a dp range, per axis. Without it the values integrate
     * forever and whatever they move sails off-screen — and the axes need
     * separate bounds, since a screen is rarely square.
     */
    public function panRange(int $minX, int $maxX, int $minY, int $maxY): static
    {
        foreach ([['x', $minX, $maxX], ['y', $minY, $maxY]] as [$axis, $min, $max]) {
            if ($min >= $max) {
                throw new InvalidArgumentException(
                    "dpad panRange {$axis} needs min < max, got {$min}-{$max}",
                );
            }
        }
        $this->panRangeX = [$minX, $maxX];
        $this->panRangeY = [$minY, $maxY];

        return $this;
    }

    /** How fast `pan()` travels, in dp per second. Default 260. */
    public function panSpeed(int $dpPerSecond): static
    {
        if ($dpPerSecond < 1 || $dpPerSecond > 4000) {
            throw new InvalidArgumentException(
                "dpad panSpeed is dp per second (1-4000), got {$dpPerSecond}",
            );
        }
        $this->panSpeed = $dpPerSecond;

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
        if (isset($attrs['diagonals'])) {
            $this->diagonals = $this->boolAttr('diagonals', $attrs['diagonals']);
        }
        if (isset($attrs['color'])) {
            $this->color = (string) $attrs['color'];
        }
        $activeColor = $attrs['activeColor'] ?? $attrs['active-color'] ?? null;
        if ($activeColor !== null) {
            $this->activeColor = (string) $activeColor;
        }
        $panX = $attrs['panX'] ?? $attrs['pan-x'] ?? null;
        $panY = $attrs['panY'] ?? $attrs['pan-y'] ?? null;
        if ($panX instanceof SharedValue || $panY instanceof SharedValue) {
            $this->pan(
                $panX instanceof SharedValue ? $panX : null,
                $panY instanceof SharedValue ? $panY : null,
            );
        }
        $speed = $attrs['panSpeed'] ?? $attrs['pan-speed'] ?? null;
        if ($speed !== null) {
            $this->panSpeed($this->intAttr('panSpeed', $speed));
        }
        $bounds = [];
        foreach (['xMin' => 'pan-x-min', 'xMax' => 'pan-x-max', 'yMin' => 'pan-y-min', 'yMax' => 'pan-y-max'] as $key => $attr) {
            $camel = 'pan'.ucfirst($key);
            $value = $attrs[$camel] ?? $attrs[$attr] ?? null;
            if ($value !== null) {
                $bounds[$key] = $this->intAttr($attr, $value);
            }
        }
        if (count($bounds) === 4) {
            $this->panRange($bounds['xMin'], $bounds['xMax'], $bounds['yMin'], $bounds['yMax']);
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

    /**
     * A Blade attribute arrives as a string, and "false" is truthy in PHP — a
     * plain cast would turn `diagonals="false"` into true and silently do nothing.
     */
    private function boolAttr(string $name, mixed $value): bool
    {
        if (is_bool($value)) {
            return $value;
        }
        $text = strtolower(trim((string) $value));
        if (in_array($text, ['false', '0', 'no', 'off'], true)) {
            return false;
        }
        if (in_array($text, ['true', '1', 'yes', 'on', ''], true)) {
            return true;
        }

        throw new InvalidArgumentException("dpad {$name} is a boolean, got '{$value}'");
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

        // Omitted props stay absent so each renderer applies its own default.
        foreach (['threshold', 'diagonalRatio', 'thickness', 'radius'] as $name) {
            if ($this->{$name} !== null) {
                $props[strtolower(preg_replace('/(?<!^)[A-Z]/', '_$0', $name))] = $this->{$name};
            }
        }
        if ($this->diagonals !== null) {
            $props['diagonals'] = $this->diagonals;
        }
        if ($this->color !== null) {
            $props['color'] = $this->color;
        }
        if ($this->activeColor !== null) {
            $props['active_color'] = $this->activeColor;
        }
        if ($this->changeMethod !== null) {
            $props['on_change'] = $registry->register($this->changeMethod);
        }
        if ($this->panX !== null) {
            $props['pan_x_id'] = $this->panX->id;
            $props['pan_x_initial'] = $this->panX->value();
        }
        if ($this->panY !== null) {
            $props['pan_y_id'] = $this->panY->id;
            $props['pan_y_initial'] = $this->panY->value();
        }
        if ($this->panSpeed !== null) {
            $props['pan_speed'] = $this->panSpeed;
        }
        if ($this->panRangeX !== null && $this->panRangeY !== null) {
            [$props['pan_x_min'], $props['pan_x_max']] = $this->panRangeX;
            [$props['pan_y_min'], $props['pan_y_max']] = $this->panRangeY;
        }

        return $props;
    }
}
