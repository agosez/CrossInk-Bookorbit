#include "FirmwareVersion.h"

#include <cstddef>
#include <cstring>

namespace firmware_version {
namespace {

constexpr char sanitisedForkBuildMarker[] = "-bookorbit.";
constexpr size_t VERSION_SEGMENT_COUNT = 4;

struct ParsedVersion {
  int segments[VERSION_SEGMENT_COUNT] = {0, 0, 0, 0};
  bool valid = false;
  bool releaseCandidate = false;
  // This fork releases as <upstream version>+bookorbit.<n>, so what distinguishes two of
  // its releases lives in semver build metadata — which is defined to carry no precedence
  // and which the numeric parse below therefore stops at. Read it out separately, or every
  // release of this fork compares equal to the one already installed and the updater
  // reports a failure rather than an update. Zero for versions without it, so an upstream
  // build and a fork build of the same version still order correctly.
  int forkBuild = 0;
};

bool isDigit(const char c) { return c >= '0' && c <= '9'; }

bool startsWithNumberAfterOptionalV(const char* version) {
  if (version == nullptr) return false;
  if ((version[0] == 'v' || version[0] == 'V') && isDigit(version[1])) return true;
  return isDigit(version[0]);
}

// The counter inside semver build metadata: the first run of digits after '+'. Returns 0
// when the version carries no metadata or no digits in it. `1.4.1+bookorbit.2-tiny` yields
// 2, and the variant suffix the firmware appends to its own version is ignored.
//
// A release build names itself with that '+' replaced by '-': scripts/git_branch.py
// sanitises the version it is handed, so the firmware built for the v1.6.1+bookorbit.1 tag
// reports 1.6.1-bookorbit.1. That form has to yield the same counter, or an up-to-date
// device reads its own counter as 0 and is offered the release it already runs.
int parseForkBuild(const char* version) {
  if (version == nullptr) return 0;
  const char* p = strchr(version, '+');
  if (p == nullptr) p = strstr(version, sanitisedForkBuildMarker);
  if (p == nullptr) return 0;
  while (*p != '\0' && !isDigit(*p)) ++p;
  int value = 0;
  while (isDigit(*p)) {
    value = value * 10 + (*p - '0');
    ++p;
  }
  return value;
}

bool containsRcMarker(const char* version) {
  if (version == nullptr) return false;
  for (const char* p = version; p[0] != '\0' && p[1] != '\0' && p[2] != '\0'; ++p) {
    if (p[0] == '-' && (p[1] == 'r' || p[1] == 'R') && (p[2] == 'c' || p[2] == 'C')) {
      return true;
    }
  }
  return false;
}

ParsedVersion parseVersion(const char* version) {
  ParsedVersion parsed;
  if (!startsWithNumberAfterOptionalV(version)) return parsed;

  const char* p = version;
  if (p[0] == 'v' || p[0] == 'V') ++p;

  size_t segmentIndex = 0;
  while (segmentIndex < VERSION_SEGMENT_COUNT) {
    if (!isDigit(*p)) return parsed;

    int value = 0;
    while (isDigit(*p)) {
      value = value * 10 + (*p - '0');
      ++p;
    }
    parsed.segments[segmentIndex] = value;
    ++segmentIndex;

    if (*p != '.') break;
    ++p;
  }

  parsed.valid = true;
  parsed.releaseCandidate = containsRcMarker(version);
  parsed.forkBuild = parseForkBuild(version);
  return parsed;
}

}  // namespace

int compare(const char* latestVersion, const char* currentVersion) {
  const ParsedVersion latest = parseVersion(latestVersion);
  const ParsedVersion current = parseVersion(currentVersion);
  if (!latest.valid || !current.valid) return 0;

  for (size_t i = 0; i < VERSION_SEGMENT_COUNT; ++i) {
    if (latest.segments[i] != current.segments[i]) {
      return latest.segments[i] > current.segments[i] ? 1 : -1;
    }
  }

  // Same upstream version: this fork's own release counter decides (see ParsedVersion).
  if (latest.forkBuild != current.forkBuild) return latest.forkBuild > current.forkBuild ? 1 : -1;

  if (current.releaseCandidate && !latest.releaseCandidate) return 1;
  return 0;
}

}  // namespace firmware_version
