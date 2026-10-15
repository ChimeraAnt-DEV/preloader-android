#include "pl/runtime/ModuleMap.hpp"

#include <cstddef>
#include <fstream>
#include <set>
#include <sstream>
#include <string>

namespace pl::runtime {
namespace {

/** True when a maps line's path column names exactly @p moduleName. */
bool LineNamesModule(std::string_view line, std::string_view moduleName) {
  // /proc/self/maps: "start-end perms offset dev inode   pathname"
  const std::size_t slash = line.find('/');
  if (slash == std::string_view::npos) {
    return false;
  }
  // Only count the executable segment: a library's code segment is mapped once per load, so this
  // counts copies rather than segments. The perms column sits between the address and the path.
  const std::size_t permsStart = line.find(' ');
  const std::size_t permsEnd = permsStart == std::string_view::npos ? std::string_view::npos
                                                                    : line.find(' ', permsStart + 1);
  if (permsStart == std::string_view::npos || permsEnd == std::string_view::npos) {
    return false;
  }
  if (line.substr(permsStart, permsEnd - permsStart).find('x') == std::string_view::npos) {
    return false;
  }

  std::string_view path = line.substr(slash);
  // Trim a trailing space or newline that some kernels pad the path column with.
  while (!path.empty() && (path.back() == ' ' || path.back() == '\n' || path.back() == '\r')) {
    path.remove_suffix(1);
  }
  const std::size_t lastSlash = path.rfind('/');
  std::string_view name = lastSlash == std::string_view::npos ? path : path.substr(lastSlash + 1);
  return name == moduleName;
}

} // namespace

int CountModuleMappingsIn(std::string_view mapsText, std::string_view moduleName) {
  if (moduleName.empty()) {
    return 0;
  }
  std::set<std::string> starts;
  std::istringstream stream{std::string(mapsText)};
  std::string line;
  while (std::getline(stream, line)) {
    if (!LineNamesModule(line, moduleName)) {
      continue;
    }
    const std::size_t dash = line.find('-');
    if (dash == std::string::npos) {
      continue;
    }
    starts.insert(line.substr(0, dash));
  }
  return static_cast<int>(starts.size());
}

int CountModuleMappings(std::string_view moduleName) {
  std::ifstream maps("/proc/self/maps");
  if (!maps) {
    return -1;
  }
  std::ostringstream buffer;
  buffer << maps.rdbuf();
  return CountModuleMappingsIn(buffer.str(), moduleName);
}

} // namespace pl::runtime
