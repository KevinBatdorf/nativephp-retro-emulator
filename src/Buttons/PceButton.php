<?php

namespace KevinBatdorf\RetroEmulator\Buttons;

/**
 * NEC PC Engine / TurboGrafx-16 buttons. Values mirror the native registry's
 * button names (native/cores/core_pce.cpp) and are drift-tested against it.
 */
enum PceButton: string
{
    case II = 'II';
    case Select = 'Select';
    case Run = 'Run';
    case Up = 'Up';
    case Down = 'Down';
    case Left = 'Left';
    case Right = 'Right';
    case I = 'I';
}
