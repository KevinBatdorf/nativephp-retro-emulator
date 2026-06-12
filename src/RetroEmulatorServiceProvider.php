<?php

namespace KevinBatdorf\RetroEmulator;

use Illuminate\Support\ServiceProvider;
use KevinBatdorf\RetroEmulator\Commands\CopyAssetsCommand;

class RetroEmulatorServiceProvider extends ServiceProvider
{
    public function register(): void
    {
        $this->app->singleton(Emulator::class, fn () => new Emulator);
    }

    public function boot(): void
    {
        if ($this->app->runningInConsole()) {
            $this->commands([
                CopyAssetsCommand::class,
            ]);
        }
    }
}
