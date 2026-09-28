#pragma once

#include <string>
#include <vector>

namespace pl::runtime {

/**
 * The parsed egg.json of an .AntEgg package, as the native loader sees it.
 *
 * Mirrors the Kotlin model in org.chimeramc.client.core.antegg.AntEggManifest. The two exist
 * because validation happens on whichever side receives the import: the launcher UI loads
 * at-rest packages through Kotlin, while the preloader loads them during a launch. Both must
 * apply the same rules or a package could be accepted in one place and rejected in the other.
 */
struct AntEggManifest {
  std::string name;
  std::string version;
  std::string author;
  std::string type;       // "native" or "script"
  std::string entry_point;
  std::vector<std::string> dependencies;

  bool IsNative() const { return type == "native"; }
  bool IsScript() const { return type == "script"; }

  /// Lowercase, dash-separated id derived from the name; a single safe path segment.
  std::string Id() const;
};

/**
 * Reads and extracts .AntEgg packages on the native side.
 *
 * The ZIP reader is built on zlib's raw inflate (the NDK ships zlib), so the preloader gains no
 * new third-party dependency for an import format — the alternative, a bundled ZIP library,
 * would be one more artifact to keep built for arm64 only.
 */
class AntEggLoader {
 public:
  /// Parses egg.json text, validating every required field. Returns false and sets `error`.
  static bool ParseManifest(const std::string& json, AntEggManifest& out, std::string& error);

  /// Reads egg.json straight out of an archive without extracting anything.
  static bool ReadManifest(const std::string& archive_path, AntEggManifest& out,
                           std::string& error);

  /**
   * Extracts an archive into `sandbox_root/<id>/`.
   *
   * Every entry is resolved and checked to stay inside the sandbox, because entry names in a ZIP
   * are attacker-controlled. The archive is extracted to a staging directory and moved into place
   * only when complete, so a truncated archive cannot leave a partial mod behind.
   */
  static bool Extract(const std::string& archive_path, const std::string& sandbox_root,
                      std::string& error);

  /// Validate, extract, and report the entry point path. Does not inject the library.
  static bool LoadMod(const std::string& archive_path, const std::string& sandbox_root,
                      std::string& error);

  /// True when the file name ends with the .AntEgg extension (case-insensitive).
  static bool LooksLikeAntEgg(const std::string& file_name);

 private:
  AntEggLoader() = delete;
};

}  // namespace pl::runtime
