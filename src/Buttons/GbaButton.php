<?php

namespace KevinBatdorf\RetroEmulator\Buttons;

/**
 * Game Boy Advance buttons. Values mirror the native registry's button names
 * (native/cores/core_gba.cpp) and are drift-tested against it.
 */
enum GbaButton: string
{
    case B = 'B';
    case Select = 'Select';
    case Start = 'Start';
    case Up = 'Up';
    case Down = 'Down';
    case Left = 'Left';
    case Right = 'Right';
    case A = 'A';
    case L = 'L';
    case R = 'R';
}
