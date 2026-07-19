# Licensing

This plugin embeds third-party code with real licensing consequences for the
apps that ship it. Read this before publishing an app built on it.

## This plugin is GPLv3 (because ares is)

The emulator core is [ares](https://ares-emu.net), licensed **GPLv3**. The
native bridge links ares in-process (JNI on Android, static on iOS) and calls
its API directly, so the compiled native library is a combined work and is
**GPLv3**. This plugin is distributed under GPLv3 accordingly.

Distributing the plugin as source (Packagist/GitHub) is exactly what the GPL
is for — no friction. The obligations that matter land on the **consuming
app**, not on this repo.

## What this means for an app that ships this plugin

An app that bundles this plugin ships ares inside its binary, so the
distributed app is a combined GPLv3 work. Practically:

- **The app must be GPLv3** — open-source, source available, no added
  restrictions on recipients.
- **Apple App Store: high-risk.** The GPL forbids a distributor from adding
  restrictions, and Apple's App Store terms (device limits, DRM) are exactly
  such restrictions. This is enforcement-driven — it only bites if a copyright
  holder objects — but it *has* happened (VLC was pulled from the App Store
  over this in 2010–11). GPL emulators (e.g. RetroArch) *do* ship on the App
  Store, but they're shipped by their own copyright holders; here you'd be
  redistributing ares, which you don't own, so any ares contributor could
  object and you couldn't relicense to fix it. If you plan to target the Apple
  App Store commercially, get the ares team's read and proper legal review
  first.
- **Google Play / F-Droid / sideload:** GPL apps live there routinely. The
  app is still fully GPLv3 (open-source).

None of this is legal advice — confirm with someone who does software
licensing before building a business or making store-distribution promises.

## Embedded GBA BIOS — Cult-of-GBA (MIT)

GBA boots on an embedded open BIOS because ares (and this plugin) ship no
copyrighted Nintendo firmware. It is the
[Cult-of-GBA BIOS](https://github.com/Cult-of-GBA/BIOS) by DenSinH and
fleroviux, **MIT-licensed** — original work, freely redistributable, embedded
with its license preserved at `native/firmware/LICENSE-cult-of-gba`. MIT is
GPLv3-compatible, so it adds no obligation beyond ares'. A dev may override it
with a real BIOS dump via `biosPath` for bit-accurate compatibility.
