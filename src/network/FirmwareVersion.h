#pragma once

// Orders firmware versions for the over-the-air updater: the firmware's own version against
// a GitHub release tag. Pure string logic, kept out of OtaUpdater.cpp (device-only) so the
// native test suite covers it.
namespace firmware_version {

// 1 when `latest` is newer than `current`, -1 when it is older, 0 when they are the same
// release or either one does not start with digits after an optional 'v'.
int compare(const char* latest, const char* current);

}  // namespace firmware_version
