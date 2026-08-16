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
    | The load order per system, one shape everywhere: the fast pick first,
    | the accurate pick as its fallback. This is the setup we recommend —
    | the fetchable engines can't ship inside the plugin (their own
    | licences; see LICENSING.md), so fetch what you want the app to
    | actually use and it plays on the next build:
    |
    |     php artisan retro-emulator:fetch-core snes9x
    |
    | An engine that isn't present is skipped with a warning (your app log
    | and the device log) and the built-in engine, ares, is the final
    | fallback — the app always boots. Fetched cores are Android-only
    | (libretro publishes no usable iOS builds), so on iOS this map's
    | fetched entries skip and ares serves. A single string works too
    | ('gb' => 'sameboy'). A per-boot backend on the system's config
    | (SfcConfig(backend: 'bsnes')) overrides this map and is strict: what
    | you name must serve, or loadSystem throws. Emulator::systems()
    | reports only the bundled engines — fetched cores never appear there.
    |
    */

    'backends' => [
        'fc' => ['fceumm', 'mesen'],
        'sfc' => ['snes9x', 'bsnes'],
        // ares first: its GB mixer (with our constant-idle patch) is the
        // click-free pick; SameBoy models the hardware's DAC steps, which pop
        // on GBDK sound drivers.
        'gb' => ['ares', 'sameboy'],
        'gbc' => ['ares', 'sameboy'],
        'gba' => ['mgba', 'ares'],
        // PicoDrive's FM voicing is audibly off; keep it an explicit pick.
        'md' => ['ares', 'picodrive'],
    ],

];
