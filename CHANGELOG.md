# Changelog

All notable changes to `kevinbatdorf/retro-emulator` are documented here.

## [Unreleased]

Initial marketplace release.

- NES, SNES, Game Boy, Game Boy Color, GBA, and Mega Drive emulation
  via ares, SameBoy, and mGBA, with optional fetchable libretro cores
  on Android (fceumm, mesen, snes9x, bsnes, picodrive, genesis_plus_gx).
- `<native:emulator>` and `<native:dpad>` EDGE elements.
- Fluent PHP API: boot/playback, save states with undo, rewind,
  fast-forward, memory read/write/watch, controllers + multitap + mouse,
  cheats, shaders (librashader), video/audio settings, screenshots,
  capability introspection.
- JavaScript bridge library exporting every bridge function for SPA
  frontends, with `onNativeEvent` for native event listening.
- Nine dispatched events, from `EmulatorStarted` to `WindowMetricsChanged`.
