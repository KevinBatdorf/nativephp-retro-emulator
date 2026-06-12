// Android replacement for ares.cpp (normally generated from ares.cpp.in).
//
// Differences from the desktop build:
//   - Resource embed (icons/logos) is a no-op stub — not needed on Android.
//   - GDB remote debugging server is omitted — not useful on-device.
//   - Version strings are inlined; no configure_file step required.

#include <ares/ares.hpp>
#include <ares/debug/debug.cpp>
#include <ares/node/node.cpp>
// scheduler/thread.cpp are header-only — included inside each system's namespace via ares/inline.hpp

// Resource stub — desktop build embeds PNG icons here; Android needs none.
namespace Resource {
    namespace Ares {
        const unsigned char* Icon1x  = nullptr;
        const unsigned int   Icon1xSize = 0;
        const unsigned char* Icon2x  = nullptr;
        const unsigned int   Icon2xSize = 0;
        const unsigned char* Logo1x  = nullptr;
        const unsigned int   Logo1xSize = 0;
        const unsigned char* Logo2x  = nullptr;
        const unsigned int   Logo2xSize = 0;
    }
}

namespace ares {

Platform* platform = nullptr;
atomic<bool> _runAhead = false;

const string Name       = "ares";
const string Version    = "v140-android";
const string Copyright  = "2004-2025 ares team, Near et al.";
const string License    = "ISC";
const string LicenseURI = "https://opensource.org/licenses/ISC";
const string Website    = "ares-emu.net";
const string WebsiteURI = "https://ares-emu.net/";
const u32    SerializerSignature = 0x31545342;  // "BST1" little-endian

}
