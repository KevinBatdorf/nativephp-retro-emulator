// Force-included (per CMake) into every ares translation unit on Android.
//
// Paks are vfs trees built in libretro_ares.so; boards read them from
// dlopen'd core modules. vfs::directory::read/write use dynamic_pointer_cast,
// and Android's libc++abi compares RTTI by pointer — a core's own weak
// typeinfo copy never equals the pak object's, so the cast (and the boot)
// silently fails with an empty ROM. These extern-template declarations stop
// cores from instantiating their own copies: they import the ones
// multi_pak.cpp explicitly instantiates inside libretro_ares.so, where the
// cast's RTTI and the pak objects' RTTI are the same definitions.
#pragma once

#include <nall/nall.hpp>
#include <nall/vfs.hpp>

// nall/serial.hpp (via nall.hpp) drags in <termios.h>, whose NCCS macro
// ("number of control chars", 19) collides with the PS1 GTE instruction
// CPU::NCCS. ares core TUs never see termios upstream — only this
// force-include introduces it, so it un-defines what it dragged in.
#ifdef NCCS
#undef NCCS
#endif

extern template std::shared_ptr<nall::vfs::file>
nall::vfs::directory::read<nall::vfs::file>(const nall::string&);
extern template std::shared_ptr<nall::vfs::file>
nall::vfs::directory::write<nall::vfs::file>(const nall::string&);
