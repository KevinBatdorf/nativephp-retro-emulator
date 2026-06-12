<?php

namespace KevinBatdorf\RetroEmulator\Facades;

use Illuminate\Support\Facades\Facade;

/**
 * @method static \KevinBatdorf\RetroEmulator\Emulator boot(string $surface)
 * @method static array<int, array{id: string, name: string, biosRequired: bool, stable: bool}> getSystems()
 *
 * @see \KevinBatdorf\RetroEmulator\Emulator
 */
class Emulator extends Facade
{
    protected static function getFacadeAccessor(): string
    {
        return \KevinBatdorf\RetroEmulator\Emulator::class;
    }
}
