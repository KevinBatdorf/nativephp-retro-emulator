// iOS stub — shadows ares/ares/n64/accuracy.hpp (found first via the iOS
// stubs/ include path, like stubs/ares/resource/resource.hpp).
//
// Why: ares' N64 CPU/RSP select the sljit recompiler at COMPILE time
// (Accuracy::CPU::Recompiler = !Interpreter, and Interpreter is false on arm64
// because recompiler::generic::supported is true). cpu.cpp then does, under
// `if constexpr(Accuracy::CPU::Recompiler)`, a 63 MiB *executable* bump
// allocation at power-on. iOS forbids allocating executable pages without the
// JIT entitlement (unavailable to ordinary apps), so that path crashes on
// device. Forcing Interpreter = true compiles the executable allocation (and
// every other recompiler `if constexpr` block) out entirely — the N64 CPU/RSP
// run the interpreter, which needs no JIT pages. (plan.md: "force interpreter
// on iOS, no JIT pages".)
//
// Mirrors upstream accuracy.hpp verbatim except the two Interpreter lines; keep
// in sync if the ares submodule's accuracy.hpp changes.
#pragma once

struct Accuracy {
  //enable all accuracy flags
  static constexpr bool Reference = 0;

  struct CPU {
    // iOS: force the interpreter (no JIT / executable pages). Upstream:
    //   Interpreter = 0 | Reference | !recompiler::generic::supported;
    static constexpr bool Interpreter = true;
    static constexpr bool Recompiler = !Interpreter;

    //Maximum number of cycles to run the CPU without synchronization
    static constexpr s64 JitInterleaving = 2048 * 2;

    //exceptions when the CPU accesses unaligned memory addresses
    static constexpr bool AddressErrors = 1 | Reference;
  };

  struct RSP {
    // iOS: force the interpreter (no JIT / executable pages). Upstream:
    //   Interpreter = 0 | Reference | !recompiler::generic::supported;
    static constexpr bool Interpreter = true;
    static constexpr bool Recompiler = !Interpreter;

    //VU instructions
    static constexpr bool SISD = 0 | Reference | !ARCHITECTURE_SUPPORTS_SSE4_1;
    static constexpr bool SIMD = !SISD;
  };

  struct RDRAM {
    static constexpr bool Broadcasting = 0;
  };

  struct PIF {
    // Emulate a region-locked console
    static constexpr bool RegionLock = false;
    // Emulate the PIF's checksum security check
    static constexpr bool IPL2Checksum = true;
  };
};
