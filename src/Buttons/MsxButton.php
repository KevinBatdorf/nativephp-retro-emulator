<?php

namespace KevinBatdorf\RetroEmulator\Buttons;

/**
 * MSX joystick buttons. Values mirror the native registry's button names
 * (native/cores/core_msx.cpp) and are drift-tested against it.
 */
enum MsxButton: string
{
    case A = 'A';
    case Up = 'Up';
    case Down = 'Down';
    case Left = 'Left';
    case Right = 'Right';
    case B = 'B';
}
