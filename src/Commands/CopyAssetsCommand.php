<?php

namespace KevinBatdorf\RetroEmulator\Commands;

use Illuminate\Filesystem\Filesystem;
use Native\Mobile\Plugins\Commands\NativePluginHookCommand;

class CopyAssetsCommand extends NativePluginHookCommand
{
    protected $signature = 'nativephp:retro-emulator:copy-assets';

    protected $description = 'Copy assets for the Retro Emulator plugin';

    public function handle(): int
    {
        if (! $this->isAndroid()) {
            return self::SUCCESS;
        }

        // Host apps don't compile the ares C++ — they consume the prebuilt
        // modular set (frontend + shared runtime + one library per core) and
        // this hook bundles only what config/retro-emulator.php selects.
        $source = $this->pluginPath().'/resources/android/jniLibs';
        $destination = $this->buildPath().'/app/src/main/jniLibs';

        if (! is_dir($source)) {
            $this->error('Prebuilt jniLibs missing — run scripts/build_android_libs.sh in the plugin repo.');

            return self::FAILURE;
        }

        $systems = config('retro-emulator.systems');
        $shaders = config('retro-emulator.shaders', true);

        if (is_array($systems)) {
            $unknown = array_diff($systems, $this->availableSystems($source));
            if ($unknown !== []) {
                $this->error(
                    'config/retro-emulator.php selects systems this plugin build does not provide: '
                    .implode(', ', $unknown)
                );

                return self::FAILURE;
            }
        }

        $files = new Filesystem;
        $bundled = [];

        foreach ($files->directories($source) as $abiDir) {
            $abi = basename($abiDir);

            foreach ($files->files($abiDir) as $file) {
                $name = $file->getFilename();

                if (! $this->shouldBundle($name, $systems, $shaders)) {
                    continue;
                }

                $files->ensureDirectoryExists("$destination/$abi");
                $files->copy($file->getPathname(), "$destination/$abi/$name");
                $bundled[$name] = true;
            }
        }

        $this->info('Bundled native libraries: '.implode(', ', array_keys($bundled)));

        return self::SUCCESS;
    }

    /** Core ids present in the prebuilt set (from any ABI's module files). */
    private function availableSystems(string $source): array
    {
        $ids = [];

        foreach (glob("$source/*/libretro_core_*.so") ?: [] as $path) {
            $ids[] = substr(basename($path, '.so'), strlen('libretro_core_'));
        }

        return array_values(array_unique($ids));
    }

    private function shouldBundle(string $name, ?array $systems, bool $shaders): bool
    {
        if ($name === 'liblibrashader.so') {
            return $shaders;
        }

        if (preg_match('/^libretro_core_(.+)\.so$/', $name, $m)) {
            return $systems === null || in_array($m[1], $systems, true);
        }

        // The frontend + shared runtime always ship.
        return true;
    }
}
