#include "pl/memory/ModulePathMatch.hpp"

#include <cctype>

namespace pl::memory {
namespace {

std::string_view Trim(std::string_view text) {
  while (!text.empty() && (text.back() == ' ' || text.back() == '\n' || text.back() == '\r' ||
                           text.back() == '\t')) {
    text.remove_suffix(1);
  }
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
    text.remove_prefix(1);
  }
  return text;
}

int HexDigit(char ch) {
  if (ch >= '0' && ch <= '9') return ch - '0';
  if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
  if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
  return -1;
}

bool ParseHex(std::string_view text, std::uintptr_t &out) {
  if (text.empty()) return false;
  std::uintptr_t value = 0;
  for (const char ch : text) {
    const int digit = HexDigit(ch);
    if (digit < 0) return false;
    value = (value << 4) | static_cast<std::uintptr_t>(digit);
  }
  out = value;
  return true;
}

} // namespace

bool ParseMapsLine(std::string_view line, MapsLine &out) {
  // "start-end perms offset dev inode   pathname"
  const std::size_t dash = line.find('-');
  if (dash == std::string_view::npos) return false;
  const std::size_t space = line.find(' ', dash);
  if (space == std::string_view::npos) return false;

  std::uintptr_t start = 0;
  std::uintptr_t end = 0;
  if (!ParseHex(line.substr(0, dash), start)) return false;
  if (!ParseHex(line.substr(dash + 1, space - dash - 1), end)) return false;
  if (end <= start) return false;

  std::size_t cursor = space;
  while (cursor < line.size() && line[cursor] == ' ') ++cursor;
  const std::size_t permsEnd = line.find(' ', cursor);
  const std::string_view perms =
      line.substr(cursor, permsEnd == std::string_view::npos ? std::string_view::npos
                                                             : permsEnd - cursor);
  if (perms.size() < 4) return false;

  out.start = start;
  out.end = end;
  for (int i = 0; i < 4; ++i) out.perms[i] = perms[static_cast<std::size_t>(i)];
  out.perms[4] = '\0';

  // After the permission column come `offset`, `dev` and `inode`, and then the pathname (which may
  // be absent, and may itself contain spaces). Skip exactly three whitespace-separated fields; what
  // remains, trimmed, is the path column -- empty for an anonymous mapping.
  std::string_view rest =
      permsEnd == std::string_view::npos ? std::string_view{} : line.substr(permsEnd + 1);
  for (int field = 0; field < 3; ++field) {
    while (!rest.empty() && (rest.front() == ' ' || rest.front() == '\t')) rest.remove_prefix(1);
    const std::size_t fieldEnd = rest.find(' ');
    if (fieldEnd == std::string_view::npos) {
      rest = std::string_view{};
      break;
    }
    rest.remove_prefix(fieldEnd + 1);
  }
  out.path = Trim(rest);
  return true;
}

bool MapsPathNamesModule(std::string_view path, std::string_view moduleName) {
  if (path.empty() || moduleName.empty()) return false;
  const std::string_view trimmed = Trim(path);
  const std::size_t lastSlash = trimmed.rfind('/');
  const std::string_view name =
      lastSlash == std::string_view::npos ? trimmed : trimmed.substr(lastSlash + 1);
  return name == moduleName;
}

bool MapsPermsExecutable(std::string_view perms) {
  return perms.size() >= 3 && perms[2] == 'x';
}

bool MapsPermsReadable(std::string_view perms) {
  return !perms.empty() && perms[0] == 'r';
}

} // namespace pl::memory
