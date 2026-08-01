<?php

namespace KevinBatdorf\RetroEmulator\Buttons;

/**
 * SNES controller buttons. Values mirror the native catalog's button names
 * (native/host/system_catalog.cpp) and are drift-tested against it.
 */
enum SfcButton: string
{
    case B = 'B';
    case Y = 'Y';
    case Select = 'Select';
    case Start = 'Start';
    case Up = 'Up';
    case Down = 'Down';
    case Left = 'Left';
    case Right = 'Right';
    case A = 'A';
    case X = 'X';
    case L = 'L';
    case R = 'R';
}
