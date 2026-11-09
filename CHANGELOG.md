# Changelog

All notable changes to `kevinbatdorf/nativephp-retro-emulator` are documented here.

## Unreleased

- Android: starting a game straight from a declarative `<native:emulator>`
  could abort the app on a cold start (`std::bad_function_call` in
  `availableSystems`). The render thread and the declarative setup thread
  both loaded the backend modules at once behind an unguarded flag, so one
  read the registry while the other was still filling it. Loading now
  happens once, and a second caller waits for it. Apps that call the
  emulator before mounting a surface, as the demo does, never hit it.
- `<native:emulator system="…">` now boots on the engines the config's
  `backends` map lists, like `loadSystem()` does. It ignored the map and ran
  every game on ares, so GBA, GB and GBC never reached mGBA or SameBoy.
- Game Boy and Game Boy Color on SameBoy no longer pop when a game parks the
  wave channel's DAC, writes NR50 or NR51, and turns the DAC back on.
  SameBoy left its internal refresh marker (0x10) as the parked sample, so
  the DAC faded back in at an out-of-range level. Link's Awakening's dialog
  did this on every text sound.
- `Controller::ignore()` takes hardware pad positions the game stops
  seeing, so the app can use buttons a console doesn't have, such as X and
  Y on a Game Boy, for its own hotkeys. On Android their key events go on
  to the app.
- `remap()` takes hardware pad positions as sources (`pad:south`,
  `pad:west`, `pad:l` and so on), so a console can use pad buttons it has
  no name for: a Game Boy's A can sit on the left face button.

## 0.1.4 — 2026-09-20

- A running game survives its view being recreated or moved to another
  window. The emulator core now lives until `stop()` or the process ends,
  pauses while no view shows it, and resumes when one does. Before this,
  navigating away and back, a recomposition, or moving the emulator to
  another display rebooted the game from the logo.

## 0.1.1 — 2026-08-30

- Package renamed to `kevinbatdorf/nativephp-retro-emulator` (was
  `kevinbatdorf/retro-emulator`) to match the repository and the
  NativePHP plugin naming convention. No code changes.

## 0.1.0 — 2026-08-30

Initial marketplace release.

- Prebuilt native cores download on the first build for each platform
  from this repo's GitHub release, sha256-pinned — the composer package
  ships no binaries, and iOS Podfile wiring is automatic.

- NES, SNES, Game Boy, Game Boy Color, GBA, and Mega Drive emulation
  via ares, SameBoy, and mGBA, with optional fetchable libretro cores
  on Android (fceumm, mesen, snes9x, bsnes, picodrive, genesis_plus_gx).
- `<native:emulator>` and `<native:dpad>` EDGE elements.
- Fluent PHP API: boot/playback, save states with undo, rewind,
  fast-forward, memory read/write/watch, controllers + multitap + mouse,
  rumble, cheats, shaders (librashader), video/audio settings,
  screenshots, capability introspection.
- OS document picker for loading ROMs from device storage
  (`Emulator::pickRom()`), with the pick copied into app storage.
- JavaScript bridge library exporting every bridge function for SPA
  frontends, with `onNativeEvent` for native event listening.
- Nine dispatched events, from `EmulatorStarted` to `WindowMetricsChanged`.
