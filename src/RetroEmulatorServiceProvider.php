<?php

namespace KevinBatdorf\RetroEmulator;

use Illuminate\Support\ServiceProvider;
use KevinBatdorf\RetroEmulator\Commands\CopyAssetsCommand;
use KevinBatdorf\RetroEmulator\Commands\FetchCoreCommand;

class RetroEmulatorServiceProvider extends ServiceProvider
{
    public function register(): void
    {
        $this->mergeConfigFrom(__DIR__.'/../config/retro-emulator.php', 'retro-emulator');

        $this->app->singleton(Emulator::class, fn () => new Emulator);
    }

    public function boot(): void
    {
        if ($this->app->runningInConsole()) {
            $this->publishes([
                __DIR__.'/../config/retro-emulator.php' => config_path('retro-emulator.php'),
            ], 'retro-emulator-config');

            $this->commands([
                CopyAssetsCommand::class,
                FetchCoreCommand::class,
            ]);
        }
    }
}
