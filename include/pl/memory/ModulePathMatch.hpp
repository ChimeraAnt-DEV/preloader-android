#pragma once

/**
 * @file ModulePathMatch.hpp
 * @brief Pure parsing and matching for `/proc/self/maps` lines.
 *
 * Resolving a signature starts by collecting the module's memory regions from `/proc/self/maps`. The
 * obvious way to decide whether a line belongs to a module is `strstr(line, moduleName)`, and it is
 * wrong: it matches a path that merely *contains* the name, so asking for `libpreloader.so` also
 * picks up `libpreloader_extra.so`, and asking for `libc.so` matches `libc.so.6`'s directory or any
 * path with `libc.so` in a parent segment. Scanning the wrong regions costs time and can resolve a
 * pattern to an address in an unrelated library.
 *
 * The match here is on the **basename** of the path column, compared exactly, so
 * `/data/app/.../lib/arm64/libpreloader.so` matches `libpreloader.so` and `libpreloader_extra.so`
 * does not.
 *
 * The parse is a pure function over one line, so it is host-testable with synthetic maps text; only
 * the caller touches `/proc`.
 */

#include <cstdint>
#include <string_view>

namespace pl::memory {

/** One parsed `/proc/self/maps` line. */
struct MapsLine {
  std::uintptr_t start = 0;
  std::uintptr_t end = 0;
  /** The four permission characters, e.g. `r-xp`. Empty when the line had none. */
  char perms[5] = {};
  /** The path column, with trailing whitespace trimmed. Empty for an anonymous mapping. */
  std::string_view path;
};

/**
 * @brief Parses one maps line.
 *
 * @return true when the line has a valid `start-end perms` head; the path may still be empty (an
 *         anonymous mapping), which the caller decides how to treat.
 */
bool ParseMapsLine(std::string_view line, MapsLine &out);

/**
 * @brief Whether a maps path column names @p moduleName exactly (by basename).
 *
 * `path` is the whole path column; the comparison is against its last `/`-separated segment. An
 * empty path or an empty module name never matches.
 */
bool MapsPathNamesModule(std::string_view path, std::string_view moduleName);

/** @brief Whether a maps permission string grants execute (`perms[2] == 'x'`). */
bool MapsPermsExecutable(std::string_view perms);

/** @brief Whether a maps permission string grants read (`perms[0] == 'r'`). */
bool MapsPermsReadable(std::string_view perms);

} // namespace pl::memory
