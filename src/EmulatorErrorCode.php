<?php

namespace KevinBatdorf\RetroEmulator;

/**
 * Every error code the native layer can raise, grouped by the channel it
 * travels (see the docblocks below). Backed by the exact wire strings native
 * emits; drift-tested against the native source like the button enums.
 *
 * The channel is chosen by category so consistency is by construction:
 *  - Programmer errors surface synchronously as an EmulatorException.
 *  - Operational outcomes surface asynchronously as an EmulatorError event.
 *  - Query failures return a value (or throw on misuse).
 */
enum EmulatorErrorCode: string
{
    // Programmer errors — the call is wrong. Thrown as EmulatorException.
    case SurfaceNotFound = 'SURFACE_NOT_FOUND';
    case UnsupportedSystem = 'UNSUPPORTED_SYSTEM';
    case SystemNotLoaded = 'SYSTEM_NOT_LOADED';
    case InvalidParameters = 'INVALID_PARAMETERS';
    case UnknownButton = 'UNKNOWN_BUTTON';
    case RewindDisabled = 'REWIND_DISABLED';

    // Transitional, iOS-only: setShader and setInputMapping are live on Android
    // but await the iOS host renderer (step 3). Removed when that lands.
    case NotImplemented = 'NOT_IMPLEMENTED';

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
     * Whether the wrapper raises this synchronously as an EmulatorException.
     * True for programmer errors and read/write misuse; false for operational
     * outcomes (they flow as an EmulatorError event) and SCREENSHOT_FAILED (a
     * null return). Driving the boundary off the code — not off "any error
     * response" — keeps operational failures off the throw path even on a
     * platform still returning them as a bridge error.
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
