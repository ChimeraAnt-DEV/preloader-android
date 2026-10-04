#include "pl/runtime/OptifineConfig.h"

#include <cctype>
#include <map>
#include <string>

namespace pl::runtime {
namespace {

constexpr const char *kItemIds[] = {
    "allocator", "render_priority", "cpu_affinity", "refresh_rate",
    "entity_culling", "particle_culling", "dynamic_render_distance",
    "callback_trimming", "oreui_stripping"};

std::string Trim(std::string_view value) {
  std::size_t begin = 0;
  std::size_t end = value.size();
  auto isSpace = [](unsigned char ch) { return std::isspace(ch) != 0; };
  while (begin < end && isSpace(static_cast<unsigned char>(value[begin]))) ++begin;
  while (end > begin && isSpace(static_cast<unsigned char>(value[end - 1]))) --end;
  return std::string(value.substr(begin, end - begin));
}

bool ParseBool(const std::map<std::string, std::string> &values,
               const std::string &key, bool fallback) {
  auto it = values.find(key);
  if (it == values.end()) return fallback;
  const std::string &v = it->second;
  return v == "1" || v == "true" || v == "on" || v == "yes";
}

int ParseInt(const std::map<std::string, std::string> &values,
             const std::string &key, int fallback) {
  auto it = values.find(key);
  if (it == values.end()) return fallback;
  try {
    return std::stoi(it->second);
  } catch (...) {
    return fallback;
  }
}

} // namespace

std::string_view OptifineItemIdAt(int index) {
  if (index < 0 || index >= kOptifineItemCount) return "unknown";
  return kItemIds[index];
}

OptifineConfig ParseOptifineConfig(std::string_view blob) {
  std::map<std::string, std::string> values;
  std::size_t pos = 0;
  while (pos <= blob.size()) {
    std::size_t nl = blob.find('\n', pos);
    std::string_view line = blob.substr(
        pos, nl == std::string_view::npos ? std::string_view::npos : nl - pos);
    std::size_t eq = line.find('=');
    if (eq != std::string_view::npos) {
      std::string key = Trim(line.substr(0, eq));
      std::string value = Trim(line.substr(eq + 1));
      if (!key.empty()) values[key] = value;
    }
    if (nl == std::string_view::npos) break;
    pos = nl + 1;
  }

  OptifineConfig config;
  config.master = ParseBool(values, "master", false);
  for (int i = 0; i < kOptifineItemCount; ++i) {
    const std::string id(OptifineItemIdAt(i));
    config.enabled[i] = ParseBool(values, "enabled." + id, false);
    config.crashCount[i] = ParseInt(values, "crash." + id, 0);
  }
  return config;
}

OptifineDecision DecideOptifineItem(const OptifineConfig &config, int index) {
  if (index < 0 || index >= kOptifineItemCount) return OptifineDecision::Off;
  if (!config.master || !config.enabled[index]) return OptifineDecision::Off;
  if (config.crashCount[index] >= kOptifineCrashLoopThreshold) {
    return OptifineDecision::AutoDisabled;
  }
  return OptifineDecision::Apply;
}

} // namespace pl::runtime
