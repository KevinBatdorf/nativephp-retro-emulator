// Force-included (per CMake) into every ares translation unit on Android.
//
// Paks are vfs trees built in libbackend_ares.so; boards read them from
// dlopen'd core modules. vfs::directory::read/write use dynamic_pointer_cast,
// and Android's libc++abi compares RTTI by pointer — a core's own weak
// typeinfo copy never equals the pak object's, so the cast (and the boot)
// silently fails with an empty ROM. These extern-template declarations stop
// cores from instantiating their own copies: they import the ones
// multi_pak.cpp explicitly instantiates inside libbackend_ares.so, where the
// cast's RTTI and the pak objects' RTTI are the same definitions.
#pragma once

#include <nall/nall.hpp>
#include <nall/vfs.hpp>

extern template std::shared_ptr<nall::vfs::file>
nall::vfs::directory::read<nall::vfs::file>(const nall::string&);
extern template std::shared_ptr<nall::vfs::file>
nall::vfs::directory::write<nall::vfs::file>(const nall::string&);
