<?php

namespace KevinBatdorf\RetroEmulator\Buttons;

/**
 * Mega Drive / Genesis buttons (libretro-style layout: A/B/C on
 * west/south/east, X/Y/Z on north/shoulders). Values mirror the native
 * catalog's button names (native/host/system_catalog.cpp) and are drift-tested
 * against it.
 */
enum MdButton: string
{
    case B = 'B';
    case A = 'A';
    case Mode = 'Mode';
    case Start = 'Start';
    case Up = 'Up';
    case Down = 'Down';
    case Left = 'Left';
    case Right = 'Right';
    case C = 'C';
    case X = 'X';
    case Y = 'Y';
    case Z = 'Z';
}
