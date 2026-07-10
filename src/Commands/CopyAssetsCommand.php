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
        // libretro_emulator.so per ABI (see scripts/build_android_libs.sh).
        $source = $this->pluginPath().'/resources/android/jniLibs';
        $destination = $this->buildPath().'/app/src/main/jniLibs';

        if (! is_dir($source)) {
            $this->error('Prebuilt jniLibs missing — run scripts/build_android_libs.sh in the plugin repo.');

            return self::FAILURE;
        }

        (new Filesystem)->copyDirectory($source, $destination);
        $this->info("Copied native libraries to {$destination}");

        return self::SUCCESS;
    }
}
