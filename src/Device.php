<?php

namespace KevinBatdorf\RetroEmulator;

/**
 * Controller devices you can connect to a port with
 * {@see Emulator::connectDevice()}. Values are the exact ares peripheral names.
 * Which devices a system accepts is reported per port by {@see Emulator::ports()}
 * ("supported"); connecting an unsupported one throws UNSUPPORTED_DEVICE.
 *
 * SNES devices beyond the standard gamepad land incrementally (step 1.5): Mouse
 * first, then the light-guns and multitap.
 */
enum Device: string
{
    case Gamepad = 'Gamepad';
    case Mouse = 'Mouse';
    case SuperScope = 'Super Scope';
    case Justifier = 'Justifier';
}
