<?php

uses()->in('.');

/*
|--------------------------------------------------------------------------
| nativephp_call stub
|--------------------------------------------------------------------------
| The real nativephp_call() is a C extension injected by the NativePHP
| runtime. In tests we define a PHP stub that reads from a global response
| registry. Tests set entries in $GLOBALS['__nativephp_mock'] before calling
| PHP API methods, then clear them afterward.
|
| Existing tests that do NOT set a mock entry receive null, which triggers
| the graceful fallback in each Emulator method — so they continue to pass.
|
| Every call is also appended to $GLOBALS['__nativephp_calls'] (decoded
| payload included) so tests can assert on outgoing bridge traffic.
*/

if (! function_exists('nativephp_call')) {
    $GLOBALS['__nativephp_mock'] = [];
    $GLOBALS['__nativephp_calls'] = [];

    function nativephp_call(string $function, string $payload): ?string
    {
        $GLOBALS['__nativephp_calls'][] = ['function' => $function, 'payload' => json_decode($payload, true)];

        return $GLOBALS['__nativephp_mock'][$function] ?? null;
    }
}
