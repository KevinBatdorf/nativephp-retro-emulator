<?php

namespace KevinBatdorf\RetroEmulator;

use FilesystemIterator;
use RecursiveDirectoryIterator;
use RecursiveIteratorIterator;

/**
 * Discovery helper for librashader `.slangp` shader presets. Point it at a
 * directory of presets — e.g. a copy of libretro's slang-shaders, which this
 * plugin does not bundle — to build a shader picker, then hand a chosen path to
 * {@see Emulator::setShader()}.
 */
final class Shaders
{
    /**
     * Every `.slangp` preset under $dir (searched recursively), as absolute
     * paths sorted alphabetically. The basename without the extension makes a
     * good display label for a picker. Returns an empty list when $dir does not
     * exist, so a missing shader directory is not an error.
     *
     * @return list<string>
     */
    public static function in(string $dir): array
    {
        if (! is_dir($dir)) {
            return [];
        }

        $presets = [];
        $files = new RecursiveIteratorIterator(
            new RecursiveDirectoryIterator($dir, FilesystemIterator::SKIP_DOTS)
        );

        foreach ($files as $file) {
            if ($file->isFile() && strtolower($file->getExtension()) === 'slangp') {
                $presets[] = $file->getPathname();
            }
        }

        sort($presets);

        return $presets;
    }
}
