<?php

namespace KevinBatdorf\RetroEmulator;

/**
 * How the surface receives hardware gamepad input, since Android delivers
 * gamepad events to the focused view and any on-screen control contends for it.
 */
enum InputCapture: string
{
    /** Pad drives the emulator only while its view holds focus (default), freeing it for the app's own focusable UI otherwise. */
    case Focus = 'focus';

    /** Pad drives the emulator regardless of focus; touch and other input still reach the app's UI. */
    case Global = 'global';
}
