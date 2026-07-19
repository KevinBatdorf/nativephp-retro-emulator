<?php

namespace KevinBatdorf\RetroEmulator\Buttons;

/**
 * Nintendo 64 controller buttons. Values mirror the native registry's button
 * names (native/cores/core_n64.cpp) and are drift-tested against it. The
 * analog stick is a separate axis input, not a button.
 */
enum N64Button: string
{
    case Up = 'Up';
    case Down = 'Down';
    case Left = 'Left';
    case Right = 'Right';
    case B = 'B';
    case A = 'A';
    case CUp = 'C-Up';
    case CDown = 'C-Down';
    case CLeft = 'C-Left';
    case CRight = 'C-Right';
    case L = 'L';
    case R = 'R';
    case Z = 'Z';
    case Start = 'Start';
}
