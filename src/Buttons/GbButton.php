<?php

namespace KevinBatdorf\RetroEmulator\Buttons;

/**
 * Game Boy buttons. Values mirror the native registry's button names
 * (native/system_registry.cpp) and are drift-tested against it.
 */
enum GbButton: string
{
    case B = 'B';
    case Select = 'Select';
    case Start = 'Start';
    case Up = 'Up';
    case Down = 'Down';
    case Left = 'Left';
    case Right = 'Right';
    case A = 'A';
}
