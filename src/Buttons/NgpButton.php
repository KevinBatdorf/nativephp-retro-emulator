<?php

namespace KevinBatdorf\RetroEmulator\Buttons;

/**
 * SNK Neo Geo Pocket inputs. Option is the console's start-equivalent; Power
 * and Debugger are real console inputs the BIOS and games read, kept
 * pressable like desktop ares does. Values mirror the native registry's
 * button names (native/cores/core_ngp.cpp) and are drift-tested against it.
 */
enum NgpButton: string
{
    case A = 'A';
    case Option = 'Option';
    case Up = 'Up';
    case Down = 'Down';
    case Left = 'Left';
    case Right = 'Right';
    case B = 'B';
    case Power = 'Power';
    case Debugger = 'Debugger';
}
