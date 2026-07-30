<?php

return [

    /*
    |--------------------------------------------------------------------------
    | Bundled systems
    |--------------------------------------------------------------------------
    |
    | The emulator cores to ship inside your app. null bundles every core the
    | plugin provides; an array of ares system ids (see Emulator::systems())
    | bundles only those — e.g. ['sfc'] for an SNES-only app. Loading a system
    | you didn't bundle throws UNSUPPORTED_SYSTEM at runtime.
    |
    */

    'systems' => null,

    /*
    |--------------------------------------------------------------------------
    | Shader runtime
    |--------------------------------------------------------------------------
    |
    | Whether to bundle the librashader runtime that powers setShader().
    | Disabling saves roughly 10 MB per Android ABI; setShader() then reports
    | SHADER_FAILED. (The iOS Metal runtime is part of the core framework and
    | is unaffected by this flag.)
    |
    */

    'shaders' => true,

    /*
    |--------------------------------------------------------------------------
    | Engine preferences
    |--------------------------------------------------------------------------
    |
    | Per system: the engines to try, in order. Each entry is tried until one
    | serves — an engine that isn't bundled or fetched is skipped, and when
    | the list runs dry the built-in engine (ares) plays. So the defaults
    | below prefer the fast engine everywhere, cost nothing until a core is
    | actually available, and activate the moment you fetch one:
    |
    |     php artisan retro-emulator:fetch-core snes9x
    |
    | A single string works too ('gb' => 'sameboy'). A per-boot backend on
    | the system's config (SfcConfig(backend: 'bsnes')) overrides this map
    | and is strict: what you name must serve, or loadSystem throws.
    | Emulator::systems() reports each system's available engines.
    |
    */

    'backends' => [
        'fc' => ['fceumm'],
        'sfc' => ['snes9x'],
        'gb' => ['sameboy'],
        'gbc' => ['sameboy'],
        'gba' => ['mgba'],
        'md' => ['picodrive'],
    ],

];
