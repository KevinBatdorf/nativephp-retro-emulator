// Minimal shim of mia's infrastructure so mia's medium analyzers
// (mia/medium/{famicom,game-boy,mega-drive}.cpp) compile unmodified.
//
// We reuse mia's analyze*() functions verbatim — they turn raw ROM bytes into
// a BML manifest (mapper/board detection, region, save-RAM layout). That logic
// is hundreds of lines of hardware knowledge we do not want to fork.
//
// The mediums' load()/save() methods also compile (they are part of the same
// translation unit) but are never called: they do disk I/O through the base
// class methods below, which are stubbed in mia_mediums.cpp. Pak assembly is
// done in-memory by MultiPak instead.
#pragma once

#include <nall/nall.hpp>
#include <nall/vfs.hpp>

using namespace nall;

// Free operator+= for nall::string, exactly as mia/mia.cpp defines it — the
// mediums' `s += {...}` manifest building resolves through nall::string's
// variadic constructor.
inline auto operator+=(nall::string& lhs, const nall::string& rhs) -> nall::string& {
  lhs.append(rhs);
  return lhs;
}

// mia/mia.cpp helper — hex-encodes bytes for manifest `data:` lines.
inline auto hexString(std::span<const u8> view) -> string {
  string s;
  for(u8 n : view) s.append(hex(n, 2L), " ");
  return s.stripRight();
}

// mia's analyzers log unknown hardware through ares' debug facility
// (`debug(unimplemented, …)`). Route to a no-op — analyzer failures surface
// through MultiPak's CartridgeResult.error instead.
struct MiaShimDebug {
  template<typename... P> auto unimplemented(P&&...) -> void {}
  template<typename... P> auto unhandled(P&&...) -> void {}
  template<typename... P> auto unusual(P&&...) -> void {}
  template<typename... P> auto unverified(P&&...) -> void {}
};
inline MiaShimDebug _miaShimDebug;
#define debug(function, ...) _miaShimDebug.function(__VA_ARGS__)

enum ResultEnum {
  successful,
  noFileSelected,
  databaseNotFound,
  romNotFoundInDatabase,
  romNotFound,
  invalidROM,
  couldNotParseManifest,
  noFirmware,
  otherError,
};

struct LoadResult {
  ResultEnum result;
  string info;

  LoadResult(ResultEnum r) : result(r) {}
  LoadResult(ResultEnum r, string i) : result(r), info(i) {}
  bool operator==(const LoadResult& other) { return result == other.result; }
  bool operator!=(const LoadResult& other) { return result != other.result; }
};

// mia/medium/mega-drive.cpp references the SVP firmware to repair Virtua
// Racing dumps that were ripped without the SVP program. Defined from the
// generated embedded firmware in mia_mediums.cpp.
namespace Resource::MegaDrive {
  extern const unsigned char* SVP;
}

namespace mia {

struct Pak {
  virtual ~Pak() = default;
  virtual auto name() -> string { return {}; }
  virtual auto extensions() -> std::vector<string> { return {}; }
  virtual auto load(string location = {}) -> LoadResult { return successful; }
  virtual auto save(string location = {}) -> bool { return true; }

  // Game name derived from a file location (used in FDS/database manifests).
  auto name(string location) const -> string { return Location::prefix(location); }

  // Disk-I/O helpers referenced by the mediums' load()/save() bodies.
  // Stubbed — never called through MultiPak's analyze-only flow.
  auto read(string location) -> std::vector<u8>;
  auto append(std::vector<u8>& data, string location) -> bool;
  auto load(string name, string extension, string location = {}) -> bool;
  auto save(string name, string extension, string location = {}) -> bool;
  auto load(Markup::Node node, string extension, string location = {}) -> bool;
  auto save(Markup::Node node, string extension, string location = {}) -> bool;

  string location;
  string manifest;
  std::shared_ptr<vfs::directory> pak;
};

struct Medium : Pak {
  // Stubbed: no game database is bundled — loadDatabase() reports success and
  // manifestDatabase() finds no match, so analyze*() always falls through to
  // pure header analysis.
  auto loadDatabase() -> bool;
  auto manifestDatabase(string sha256) -> string;

  string sha256;
};

struct Cartridge : Medium {
  auto type() -> string { return "Cartridge"; }
};

} // namespace mia
