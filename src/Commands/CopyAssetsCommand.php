<?php

namespace KevinBatdorf\RetroEmulator\Commands;

use Native\Mobile\Plugins\Commands\NativePluginHookCommand;

class CopyAssetsCommand extends NativePluginHookCommand
{
    protected $signature = 'nativephp:retro-emulator:copy-assets';

    protected $description = 'Copy assets for the Retro Emulator plugin';

    public function handle(): int
    {
        return self::SUCCESS;
    }
}
