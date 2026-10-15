#pragma once

/**
 * @file ModuleMap.hpp
 * @brief Counts how many times a shared library is mapped into this process.
 *
 * <p>A shared library loaded twice has two independent copies of its static state. For the preloader
 * that is fatal and silent: the launcher's JNI calls would publish into one copy's registry while the
 * render hook running inside the other copy draws from its own (empty) registry, so cosmetics never
 * appear and nothing crashes. Counting the mappings of {@code libpreloader.so} in
 * {@code /proc/self/maps} is the cheapest proof that only one copy exists.
 *
 * <p>The parse is a pure function over the maps text so it can be unit-tested with a synthetic file;
 * only {@link CountModuleMappings} touches {@code /proc}.
 */

#include <string>
#include <string_view>

namespace pl::runtime {

/**
 * @brief Counts the loaded copies of a module in a {@code /proc/self/maps} body.
 *
 * <p>One load maps several segments (read-execute, read-only, read-write), so counting lines would
 * over-report. A shared library's executable segment is mapped exactly once per load, so the count
 * is of distinct start addresses among the module's {@code *x*} mappings — 1 for a normally loaded
 * copy, 2 when the same library has been loaded twice. The match is on the path-name column ending
 * with {@code /<moduleName>} (or exactly {@code moduleName}), so
 * {@code /data/app/.../lib/arm64/libpreloader.so} counts and {@code libpreloader_extra.so} does not.
 */
int CountModuleMappingsIn(std::string_view mapsText, std::string_view moduleName);

/** @brief Counts the mappings of @p moduleName in this process, or -1 when unreadable. */
int CountModuleMappings(std::string_view moduleName);

} // namespace pl::runtime
