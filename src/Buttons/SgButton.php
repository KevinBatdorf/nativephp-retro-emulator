<?php

namespace KevinBatdorf\RetroEmulator\Buttons;

/**
 * Sega SG-1000 buttons. Values mirror the native registry's button names
 * (native/cores/core_sg.cpp) and are drift-tested against it.
 */
enum SgButton: string
{
    case One = '1';
    case Two = '2';
    case Up = 'Up';
    case Down = 'Down';
    case Left = 'Left';
    case Right = 'Right';
}
