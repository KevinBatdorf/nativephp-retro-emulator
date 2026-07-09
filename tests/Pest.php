<?php

/*
|--------------------------------------------------------------------------
| Test Case
|--------------------------------------------------------------------------
*/

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
*/

if (! function_exists('nativephp_call')) {
    $GLOBALS['__nativephp_mock'] = [];

    function nativephp_call(string $function, string $payload): ?string
    {
        return $GLOBALS['__nativephp_mock'][$function] ?? null;
    }
}
