<?php

namespace KevinBatdorf\RetroEmulator\Buttons;

/**
 * Atari 2600 inputs: the joystick's five, plus the console switches — many
 * games start on Reset and pick a variation on Select, so the switches press
 * like buttons (on port 1). Values mirror the native registry's button names
 * (native/cores/core_a26.cpp) and are drift-tested against it.
 */
enum A26Button: string
{
    case Fire = 'Fire';
    case Up = 'Up';
    case Down = 'Down';
    case Left = 'Left';
    case Right = 'Right';
    case Reset = 'Reset';
    case Select = 'Select';
    case LeftDifficulty = 'Left Difficulty';
    case RightDifficulty = 'Right Difficulty';
    case TvType = 'TV Type';
}
