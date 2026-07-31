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

        $this->bundleDroppedInCores($files, $destination);
        $this->warnAboutMissingPreferredEngines($destination);

        return self::SUCCESS;
    }

    /**
     * A preferred engine that isn't packaged falls through silently at
     * boot — warn at build time with the fetch command that fixes it.
     */
    private function warnAboutMissingPreferredEngines(string $destination): void
    {
        // The engines compiled into the plugin's own libraries; anything
        // else in the map must be a packaged libretro core to serve.
        $shipped = ['ares', 'sameboy', 'mgba'];

        foreach ((array) config('retro-emulator.backends', []) as $system => $engines) {
            foreach ((array) $engines as $engine) {
                $engine = $engine instanceof \KevinBatdorf\RetroEmulator\Backend
                    ? $engine->value
                    : (string) $engine;

                if (in_array($engine, $shipped, true)) {
                    break;   // present — nothing before it went missing
                }

                if (glob("{$destination}/*/lib{$engine}_libretro_android.so") !== []) {
                    break;   // packaged core — this entry serves
                }

                $this->warn(
                    "retro-emulator: config prefers '{$engine}' for {$system} but no such core is packaged — "
                    ."boots fall back. Run: php artisan retro-emulator:fetch-core {$engine}"
                );
            }
        }
    }

    /**
     * Bring-your-own libretro cores: everything under the app's
     * resources/emulator-cores/android/<abi>/ ships into jniLibs. Files gain
     * the "lib" prefix Android packaging requires; the native loader probes
     * lib<core>_libretro_android.so when a config names the core.
     */
    private function bundleDroppedInCores(Filesystem $files, string $destination): void
    {
        $source = resource_path('emulator-cores/android');

        if (! is_dir($source)) {
            return;
        }

        foreach ($files->directories($source) as $abiDir) {
            $abi = basename($abiDir);

            foreach ($files->files($abiDir) as $file) {
                $name = $file->getFilename();

                if (! str_ends_with($name, '.so')) {
                    continue;
                }

                $target = str_starts_with($name, 'lib') ? $name : "lib{$name}";
                $files->ensureDirectoryExists("$destination/$abi");
                $files->copy($file->getPathname(), "$destination/$abi/$target");
                $this->info("Bundled libretro core: {$name} ({$abi}) — ".self::coreLicenceNote($name));
            }
        }
    }

    /** One honest line per core so a licence never ships unnoticed. */
    public static function coreLicenceNote(string $filename): string
    {
        $core = preg_replace('/^lib|_libretro.*$/', '', basename($filename, '.so'));

        $known = [
            'snes9x' => 'Snes9x licence: non-commercial use only',
            'snes9x2010' => 'Snes9x licence: non-commercial use only',
            'bsnes' => 'GPL-3.0',
            'fceumm' => 'GPL-2.0',
            'nestopia' => 'GPL-2.0',
            'mesen' => 'GPL-3.0',
            'quicknes' => 'LGPL-2.1',
            'gambatte' => 'GPL-2.0',
            'genesis_plus_gx' => 'non-commercial licence',
            'picodrive' => 'non-commercial (MAME-style) licence',
        ];

        $licence = $known[$core] ?? 'licence unknown';

        return "{$licence} — you are responsible for complying when distributing your app";
    }

    /** Core ids present in the prebuilt set (from any ABI's module files). */
    private function availableSystems(string $source): array
    {
        $ids = [];

        foreach (glob("$source/*/libares_core_*.so") ?: [] as $path) {
            $ids[] = substr(basename($path, '.so'), strlen('libares_core_'));
        }

        return array_values(array_unique($ids));
    }

    private function shouldBundle(string $name, ?array $systems, bool $shaders): bool
    {
        if ($name === 'liblibrashader.so') {
            return $shaders;
        }

        if (preg_match('/^libares_core_(.+)\.so$/', $name, $m)) {
            return $systems === null || in_array($m[1], $systems, true);
        }

        // The frontend + the backend libraries always ship.
        return true;
    }
}
