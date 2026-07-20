<?php

namespace KevinBatdorf\RetroEmulator\Facades;

use Illuminate\Support\Facades\Facade;

/**
 * @method static \KevinBatdorf\RetroEmulator\Emulator surface(string $name = 'main')
 * @method static array<int, array{id: string, name: string, stable: bool, supported: bool}> systems()
 * @method static array<int, string> inputDevices()
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
