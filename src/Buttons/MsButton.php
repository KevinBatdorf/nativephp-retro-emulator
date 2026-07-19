<?php

namespace KevinBatdorf\RetroEmulator\Buttons;

/**
 * Sega Master System buttons. Pause is the console button (it raises the NMI
 * games use for pause menus) and reads/presses on port 1. Values mirror the
 * native registry's button names (native/cores/core_ms.cpp) and are
 * drift-tested against it.
 */
enum MsButton: string
{
    case One = '1';
    case Two = '2';
    case Up = 'Up';
    case Down = 'Down';
    case Left = 'Left';
    case Right = 'Right';
    case Pause = 'Pause';
}
