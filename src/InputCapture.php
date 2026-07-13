<?php

namespace KevinBatdorf\RetroEmulator;

/**
 * How the surface receives hardware gamepad input, since Android delivers
 * gamepad events to the focused view and any on-screen control contends for it.
 */
enum InputCapture: string
{
    /**
     * The emulator receives gamepad input only while its view holds focus
     * (default). Leaves the pad free to drive the app's own focusable UI —
     * menus, buttons — when those are focused.
     */
    case Focus = 'focus';

    /**
     * The emulator grabs hardware gamepad/joystick events at the window,
     * regardless of focus; touch and other input still reach the app's UI.
     * Use when the pad should always drive the game.
     */
    case Global = 'global';
}
