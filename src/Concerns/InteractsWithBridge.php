<?php

namespace KevinBatdorf\RetroEmulator\Concerns;

use KevinBatdorf\RetroEmulator\EmulatorErrorCode;
use KevinBatdorf\RetroEmulator\EmulatorException;

trait InteractsWithBridge
{
    /**
     * Send a bridge call and route its outcome to the right channel.
     *
     * A programmer-error bridge response (status "error") throws an
     * EmulatorException synchronously — without this, fluent commands discard
     * the native return and a bad call reaches the dev nowhere. SCREENSHOT_FAILED
     * is the one error that does not throw; screenshot() returns null instead.
     * Operational failures (a missing ROM, a failed save) never arrive here as
     * errors — native dispatches them as EmulatorError events.
     *
     * Returns the decoded response, or null when the runtime is absent.
     *
     * @param  array<string, mixed>  $payload
     * @return array<string, mixed>|null
     */
    protected function call(string $function, array $payload): ?array
    {
        if (! function_exists('nativephp_call')) {
            return null;
        }

        $raw = nativephp_call($function, json_encode($payload));
        $decoded = $raw === null ? null : json_decode($raw, true);

        if (is_array($decoded) && ($decoded['status'] ?? null) === 'error') {
            $code = EmulatorErrorCode::from($decoded['code']);
            if ($code->throwsAsException()) {
                throw new EmulatorException($code, $decoded['message'] ?? '');
            }
        }

        return is_array($decoded) ? $decoded : null;
    }
}
