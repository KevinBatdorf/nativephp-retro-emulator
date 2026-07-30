<?php

namespace KevinBatdorf\RetroEmulator;

/**
 * Error codes the native layer can raise, as the exact wire strings it emits
 * (drift-tested against the native source, like the button enums). Grouped by
 * how each reaches the caller:
 *  - Programmer errors throw synchronously as an EmulatorException.
 *  - Operational outcomes arrive as an EmulatorError event, never thrown.
 *  - Query failures return a value, or throw on misuse.
 */
enum EmulatorErrorCode: string
{
    // Programmer errors — the call is wrong. Thrown as EmulatorException.
    case SurfaceNotFound = 'SURFACE_NOT_FOUND';
    case UnsupportedSystem = 'UNSUPPORTED_SYSTEM';
    case UnsupportedBackend = 'UNSUPPORTED_BACKEND';
    case UnsupportedDevice = 'UNSUPPORTED_DEVICE';
    case UnsupportedOption = 'UNSUPPORTED_OPTION';
    case SystemNotLoaded = 'SYSTEM_NOT_LOADED';
    case InvalidParameters = 'INVALID_PARAMETERS';
    case UnknownButton = 'UNKNOWN_BUTTON';
    case RewindDisabled = 'REWIND_DISABLED';
    case BootOnlyOption = 'BOOT_ONLY_OPTION';

    // Operational outcomes — the call was valid but the world said no.
    // Dispatched as an EmulatorError event, never thrown.
    case RomNotFound = 'ROM_NOT_FOUND';
    case InvalidRom = 'INVALID_ROM';
    case LoadFailed = 'LOAD_FAILED';
    case SlotEmpty = 'SLOT_EMPTY';
    case SaveFailed = 'SAVE_FAILED';
    case UndoFailed = 'UNDO_FAILED';
    case RewindFailed = 'REWIND_FAILED';
    case InvalidCheat = 'INVALID_CHEAT';
    case CheatFailed = 'CHEAT_FAILED';
    case ShaderFailed = 'SHADER_FAILED';

    // Query failures. READ/WRITE throw on misuse (bad address / no core);
    // SCREENSHOT returns null instead of throwing.
    case ReadFailed = 'READ_FAILED';
    case WriteFailed = 'WRITE_FAILED';
    case ScreenshotFailed = 'SCREENSHOT_FAILED';

    /**
     * Whether the wrapper throws this as an EmulatorException. True for
     * programmer errors and read/write misuse; false for operational outcomes
     * (an EmulatorError event) and SCREENSHOT_FAILED (null return). Keying off
     * the code keeps operational failures off the throw path even if a platform
     * still returns them as a bridge error.
     */
    public function throwsAsException(): bool
    {
        return match ($this) {
            self::RomNotFound, self::InvalidRom, self::LoadFailed,
            self::SlotEmpty, self::SaveFailed, self::UndoFailed,
            self::RewindFailed, self::InvalidCheat, self::CheatFailed,
            self::ShaderFailed, self::ScreenshotFailed => false,
            default => true,
        };
    }
}
