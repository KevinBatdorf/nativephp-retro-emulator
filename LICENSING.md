# Licensing

This plugin's own code — the PHP / Kotlin / Swift bridge and build glue — is
**MIT** (see `LICENSE`). You can ship an app built on this plugin under any
license you choose, including on the Apple App Store: none of the bundled
components is copyleft in a way that reaches your app's own code.

## Bundled third-party components

The native binaries this plugin ships (`resources/android/jniLibs/*.so` and the
iOS `RetroEmulator.xcframework`) statically include the ares emulator core and
its dependencies. These are third-party works under their own licenses — all
permissive, except librashader which is file-level copyleft. Their copyright and
permission notices ship with the plugin in the files noted below, which
satisfies their attribution terms for binary redistribution.

| Component | Role | License | Notice |
|---|---|---|---|
| [ares](https://ares-emu.net) | emulator core | ISC | `THIRD-PARTY-NOTICES.md` |
| librashader | shader runtime | MPL-2.0 | `THIRD-PARTY-NOTICES.md` (librashader section) |
| sljit · libchdr · ymfm · xxHash | ares dependencies | BSD | `THIRD-PARTY-NOTICES.md` |
| zlib · LZMA SDK · qoi | ares dependencies | zlib · public-domain · MIT | `THIRD-PARTY-NOTICES.md` |
| [Cult-of-GBA BIOS](https://github.com/Cult-of-GBA/BIOS) | embedded GBA BIOS | MIT | `THIRD-PARTY-NOTICES.md` |

`THIRD-PARTY-NOTICES.md` reproduces the full aggregate notice verbatim and is a
tracked file at the package root — the `ares/` submodule and `native/` tree are
excluded from dist installs (.gitattributes export-ignore), so the notices must
not live only there or a packagist install would ship the binaries without
them. Sources of truth remain `ares/LICENSE` and
`native/firmware/LICENSE-cult-of-gba`; regenerate the notices file after an
ares submodule bump.

### librashader (MPL-2.0)

The Mozilla Public License is file-level copyleft: it governs librashader's own
source, not your application. We build it **unmodified** from a tagged release
of <https://github.com/SnowflakePowered/librashader>; that repository is the
corresponding source under MPL-2.0 § 3.2.

## Embedded GBA BIOS — Cult-of-GBA (MIT)

GBA boots on an embedded open BIOS because ares (and this plugin) ship no
copyrighted Nintendo firmware. It is the
[Cult-of-GBA BIOS](https://github.com/Cult-of-GBA/BIOS) by DenSinH and
fleroviux, **MIT-licensed** — original work, freely redistributable, embedded
with its license preserved at `native/firmware/LICENSE-cult-of-gba`. A dev may
override it with a real BIOS dump via `biosPath` for bit-accurate compatibility.

---

This is not legal advice. If you're building a business on store distribution,
have someone who does software licensing review your specific case.
