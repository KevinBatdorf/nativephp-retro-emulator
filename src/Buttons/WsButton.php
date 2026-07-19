<?php

namespace KevinBatdorf\RetroEmulator\Buttons;

/**
 * Bandai WonderSwan buttons. X1-X4 is the d-pad diamond (up/right/down/left),
 * Y1-Y4 the second diamond. Values mirror the native registry's button names
 * (native/cores/core_ws.cpp) and are drift-tested against it.
 */
enum WsButton: string
{
    case B = 'B';
    case Y1 = 'Y1';
    case Start = 'Start';
    case X1 = 'X1';
    case X3 = 'X3';
    case X4 = 'X4';
    case X2 = 'X2';
    case A = 'A';
    case Y2 = 'Y2';
    case Y3 = 'Y3';
    case Y4 = 'Y4';
    case Volume = 'Volume';
}
