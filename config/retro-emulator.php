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
    | Default engines
    |--------------------------------------------------------------------------
    |
    | Which emulation engine serves each system, e.g. ['gb' => 'ares']. Systems
    | not listed use the plugin's default: the bundled fast core where one
    | exists (SameBoy for gb/gbc), ares otherwise. A per-boot backend on the
    | system's config (GbConfig(backend: Backend::Ares)) overrides both.
    | Emulator::systems() reports each system's available backends.
    |
    */

    'backends' => [],

];
