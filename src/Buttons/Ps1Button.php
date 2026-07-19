<?php

namespace KevinBatdorf\RetroEmulator\Buttons;

/**
 * PlayStation Digital Gamepad buttons. Values mirror the native registry's
 * button names (native/cores/core_ps1.cpp) and are drift-tested against it.
 * DualShock adds L3/R3 + analog sticks — connect it via Device::DualShock.
 */
enum Ps1Button: string
{
    case Cross = 'Cross';
    case Square = 'Square';
    case Select = 'Select';
    case Start = 'Start';
    case Up = 'Up';
    case Down = 'Down';
    case Left = 'Left';
    case Right = 'Right';
    case Circle = 'Circle';
    case Triangle = 'Triangle';
    case L1 = 'L1';
    case R1 = 'R1';
    case L2 = 'L2';
    case R2 = 'R2';
}
