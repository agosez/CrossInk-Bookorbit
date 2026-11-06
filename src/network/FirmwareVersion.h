#pragma once

// Orders firmware versions for the over-the-air updater: the firmware's own version against
// a GitHub release tag, and release tags against each other. Pure string logic, kept out of
// OtaUpdater.cpp (device-only) so the native test suite covers it.
namespace firmware_version {

// True when `version` has the digits-after-an-optional-'v' form compare() reads. A tag
// without it, such as the release-candidate workflow's `rc-<branch>-<hash>`, has no place
// in the order and is never offered as an update.
bool isComparable(const char* version);

// 1 when `latest` is newer than `current`, -1 when it is older, 0 when they are the same
// release or either one is not comparable.
int compare(const char* latest, const char* current);

}  // namespace firmware_version
