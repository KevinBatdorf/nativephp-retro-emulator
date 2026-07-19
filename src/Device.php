<?php

namespace KevinBatdorf\RetroEmulator;

/**
 * Controller devices you can connect to a port with
 * {@see Emulator::connectDevice()}. Values are the exact ares peripheral names.
 * Which devices a system accepts is reported per port by {@see Emulator::ports()}
 * ("supported"); connecting an unsupported one throws UNSUPPORTED_DEVICE.
 */
enum Device: string
{
    case Gamepad = 'Gamepad';
    case Mouse = 'Mouse';
    case SuperScope = 'Super Scope';
    case Justifier = 'Justifier';
    case SuperMultitap = 'Super Multitap';
    case DigitalGamepad = 'Digital Gamepad';
    case DualShock = 'DualShock';
}
