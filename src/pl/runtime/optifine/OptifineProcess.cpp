#include "pl/runtime/optifine/OptifineProcess.h"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <sched.h>
#include <string>
#include <vector>

#include "pl/runtime/optifine/OptifineDeviceRules.h"

namespace pl::runtime {
namespace {

std::string Lower(std::string value) {
  for (char &c : value) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return value;
}

/** Reads a whole small file into a string, or empty on failure. */
std::string ReadSmallFile(const std::string &path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) return {};
  return std::string((std::istreambuf_iterator<char>(file)),
                     std::istreambuf_iterator<char>());
}

bool IsAllDigits(const char *value) {
  if (value == nullptr || *value == '\0') return false;
  for (const char *p = value; *p != '\0'; ++p) {
    if (!std::isdigit(static_cast<unsigned char>(*p))) return false;
  }
  return true;
}

} // namespace

std::string ThreadName(pid_t tid) {
  const std::string path = "/proc/self/task/" + std::to_string(tid) + "/comm";
  std::string name = ReadSmallFile(path);
  while (!name.empty() && (name.back() == '\n' || name.back() == '\r' ||
                           name.back() == ' ')) {
    name.pop_back();
  }
  return name;
}

pid_t FindThreadByName(const std::string &needle) {
  if (needle.empty()) return -1;
  const std::string loweredNeedle = Lower(needle);

  DIR *dir = opendir("/proc/self/task");
  if (dir == nullptr) return -1;

  pid_t found = -1;
  struct dirent *entry = nullptr;
  while ((entry = readdir(dir)) != nullptr) {
    if (!IsAllDigits(entry->d_name)) continue;
    const pid_t tid = static_cast<pid_t>(std::atoi(entry->d_name));
    const std::string name = ThreadName(tid);
    if (!name.empty() && Lower(name).find(loweredNeedle) != std::string::npos) {
      found = tid;
      break;
    }
  }
  closedir(dir);
  return found;
}

bool SetThreadScheduling(pid_t tid, int policy, int priority) {
  sched_param param{};
  param.sched_priority = priority;
  return sched_setscheduler(tid, policy, &param) == 0;
}

bool GetThreadScheduling(pid_t tid, int &policy, int &priority) {
  sched_param param{};
  const int result = sched_getparam(tid, &param);
  if (result != 0) return false;
  policy = sched_getscheduler(tid);
  priority = param.sched_priority;
  return true;
}

int ReadCpuMaxFrequencies(unsigned long long *out, int maxCores) {
  if (out == nullptr || maxCores <= 0) return 0;
  int count = 0;
  for (int core = 0; core < maxCores; ++core) {
    const std::string path = "/sys/devices/system/cpu/cpu" + std::to_string(core) +
                             "/cpufreq/cpuinfo_max_freq";
    const std::string text = ReadSmallFile(path);
    if (text.empty()) break; // CPUs are numbered densely; a gap means we are done.
    try {
      out[count++] = std::stoull(text);
    } catch (...) {
      out[count++] = 0;
    }
  }
  return count;
}

int ReadPerformanceCoreIndices(int *out, int maxCores) {
  if (out == nullptr || maxCores <= 0) return 0;

  unsigned long long freq[kOptifineMaxCores] = {};
  const int read = ReadCpuMaxFrequencies(freq, kOptifineMaxCores);
  if (read <= 0) return 0;

  unsigned long long fastest = 0;
  for (int i = 0; i < read; ++i) {
    if (freq[i] > fastest) fastest = freq[i];
  }
  if (fastest == 0) return 0;

  int count = 0;
  for (int i = 0; i < read && count < maxCores; ++i) {
    if (freq[i] == 0) continue;
    if (freq[i] * 10ULL >= fastest * 9ULL) {
      out[count++] = i;
    }
  }
  return count;
}

bool SetThreadAffinity(pid_t tid, unsigned long long mask) {
  if (mask == 0) return false;
  cpu_set_t set;
  CPU_ZERO(&set);
  for (int core = 0; core < kOptifineMaxCores; ++core) {
    if ((mask & (1ULL << core)) != 0) {
      CPU_SET(core, &set);
    }
  }
  return sched_setaffinity(tid, sizeof(set), &set) == 0;
}

bool CanUseRealtimeScheduling() {
  // The kernel exposes the process's permitted RT priority range in
  // /proc/sys/kernel/sched_rt_runtime_us only indirectly; the practical probe is whether a
  // SCHED_FIFO request is permitted at all, which SetThreadScheduling reports. This helper
  // answers the coarser question the UI needs: is real-time scheduling compiled in and not
  // disabled by the runtime limit.
  const std::string runtime = ReadSmallFile("/proc/sys/kernel/sched_rt_runtime_us");
  if (runtime.empty()) return true; // Not exposed; let the actual syscall decide.
  try {
    return std::stoll(runtime) != -1;
  } catch (...) {
    return true;
  }
}

} // namespace pl::runtime
