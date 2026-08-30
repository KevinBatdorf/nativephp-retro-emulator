<?php

namespace KevinBatdorf\RetroEmulator\Support;

/**
 * NativePHP's pod-injection emits only name/version pods — a vendored
 * xcframework needs a :path pod, which the copy-assets hook writes itself.
 */
class Podfile
{
    /** Null means both "already present" and "no anchor" — callers must tell them apart. */
    public static function ensurePod(string $contents, string $pluginPath): ?string
    {
        if (str_contains($contents, "pod 'RetroEmulator'")) {
            return null;
        }

        $line = "  pod 'RetroEmulator', :path => '{$pluginPath}'";

        foreach (['# NATIVEPHP_PLUGIN_PODS_END', 'def shared_pods'] as $anchor) {
            $updated = self::insertAfterLineContaining($contents, $anchor, $line);

            if ($updated !== null) {
                return $updated;
            }
        }

        return null;
    }

    public static function relativePath(string $from, string $to): string
    {
        $fromParts = array_values(array_filter(explode('/', $from), strlen(...)));
        $toParts = array_values(array_filter(explode('/', $to), strlen(...)));

        while ($fromParts !== [] && $toParts !== [] && $fromParts[0] === $toParts[0]) {
            array_shift($fromParts);
            array_shift($toParts);
        }

        return implode('/', [...array_fill(0, count($fromParts), '..'), ...$toParts]);
    }

    private static function insertAfterLineContaining(string $contents, string $needle, string $line): ?string
    {
        $lines = explode("\n", $contents);

        foreach ($lines as $i => $existing) {
            if (str_contains($existing, $needle)) {
                array_splice($lines, $i + 1, 0, [$line]);

                return implode("\n", $lines);
            }
        }

        return null;
    }
}
