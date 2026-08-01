<?php

uses()->in('.');

/*
|--------------------------------------------------------------------------
| Minimal container config
|--------------------------------------------------------------------------
| Outside a booted Laravel app the container has no 'config' binding, so
| bind the plugin's real config file.
*/

use Illuminate\Config\Repository;
use Illuminate\Container\Container;

Container::getInstance()->instance('config', new Repository([
    'retro-emulator' => require __DIR__.'/../config/retro-emulator.php',
]));

/*
|--------------------------------------------------------------------------
| nativephp_call stub
|--------------------------------------------------------------------------
| The real nativephp_call() is a C extension injected by the NativePHP
| runtime. In tests we define a PHP stub that reads from a global response
| registry. Tests set entries in $GLOBALS['__nativephp_mock'] before calling
| PHP API methods, then clear them afterward.
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
