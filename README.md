# Retro Emulator for NativePHP Mobile

> Work in progress. This README is a temporary checklist — proper documentation
> comes once the feature set is complete.

Wraps the [ares](https://ares-emu.net) multi-system emulator as a NativePHP
Mobile plugin: one prebuilt native binary, a typed and fluent PHP API, rendering
in-layout through a `<native:emulator>` element.

## Done (SNES, verified on the AYN Thor)

- [x] ROM-first boot with automatic region resolution (PAL runs at PAL speed)
- [x] pause / resume / stop
- [x] save states (+ undo) and battery saves
- [x] memory read (sync + async), write, and watches
- [x] cheats — raw `ADDR:VALUE` format, `.cheats.bml` import
- [x] run-ahead
- [x] rewind
- [x] dynamic rate control
- [x] fast-forward and speed control
- [x] video presentation (luminance, saturation, gamma, colorBleed, overscan, output, fixedScale, aspectCorrection)
- [x] volume / balance, deepBlackBoost
- [x] screenshots
- [x] status / ports / region / systems queries
- [x] software + hardware controller input
- [x] shaders — librashader `.slangp` presets (Android, Vulkan)
- [x] declarative `<native:emulator>` element (boots on mount)
- [x] typed + fluent PHP API with a global `Config`

## Not built yet

- [ ] input remapping
- [ ] peripherals (Super Scope, Justifier, mouse, multitap, …)
- [ ] debugger / tracer
- [ ] slotted media (SuFami Turbo, Satellaview)
- [ ] iOS host renderer (and iOS shaders)
- [ ] systems beyond SNES (fc / gb / md are compiled; the rest are pending)
