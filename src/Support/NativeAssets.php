<?php

namespace KevinBatdorf\RetroEmulator\Support;

use RuntimeException;
use ZipArchive;

/**
 * The prebuilt cores exceed git/dist limits, so they ship as GitHub release
 * assets that resources/native-assets.json pins by sha256.
 */
class NativeAssets
{
    public static function manifest(string $pluginPath): array
    {
        $path = $pluginPath.'/resources/native-assets.json';
        $manifest = is_file($path) ? json_decode((string) file_get_contents($path), true) : null;

        if (! is_array($manifest)) {
            throw new RuntimeException("Unreadable native asset manifest: {$path}");
        }

        return $manifest;
    }

    public static function missing(string $pluginPath, string $platform): array
    {
        $assets = self::manifest($pluginPath)['assets'] ?? [];

        return array_filter(
            $assets,
            fn (array $spec) => ($spec['platform'] ?? null) === $platform
                && ! file_exists($pluginPath.'/'.$spec['probe'])
        );
    }

    public static function url(array $manifest, string $name): string
    {
        return "{$manifest['base_url']}/{$manifest['release']}/{$name}";
    }

    public static function install(string $pluginPath, array $spec, string $zipPath): bool
    {
        if (hash_file('sha256', $zipPath) !== $spec['sha256']) {
            return false;
        }

        $zip = new ZipArchive;

        if ($zip->open($zipPath) !== true) {
            return false;
        }

        $destination = $pluginPath.'/'.$spec['extract_to'];

        if (! is_dir($destination) && ! mkdir($destination, 0755, true)) {
            $zip->close();

            return false;
        }

        $extracted = $zip->extractTo($destination);
        $zip->close();

        return $extracted && file_exists($pluginPath.'/'.$spec['probe']);
    }
}
