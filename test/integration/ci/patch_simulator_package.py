#!/usr/bin/env python3
"""Patch the fetched crossink-simulator package with shims the firmware needs.

The simulator package (lib_deps: uxjulia/crossink-simulator, unpinned) trails the
firmware's HAL surface. Until the fixes land upstream, a cold build — CI, or a
fresh checkout — needs these shims applied to the fetched copy under
.pio/libdeps/<env>/simulator/. Each patch checks for its own marker first, so
the script is idempotent and turns into a no-op as upstream catches up.

Usage: python3 test/integration/ci/patch_simulator_package.py [env...]
       (default env: simulator)

Run `pio pkg install -e <env>` first so the package exists.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]

HALCLOCK_METHODS = """\
  bool hasCurrentTime() const { return true; }
  bool formatCurrentTime(char *buf, size_t bufSize, uint8_t, bool use12Hour) const {
    if (!buf || bufSize == 0) return false;
    time_t now = time(nullptr);
    struct tm t;
    localtime_r(&now, &t);
    strftime(buf, bufSize, use12Hour ? "%I:%M" : "%H:%M", &t);
    return true;
  }
  bool syncSystemTimeFromNTP() { return false; }
"""

FIRMWARE_FLASH_STUBS = """\
Result validateOpenImageFile(HalFile &, size_t) {
  LOG_DBG(
      "FLASH",
      "[SIM] Firmware image validation is disabled in the native simulator");
  return Result::WRITE_FAIL;
}

Result flashValidatedFile(HalFile &, ProgressCb onProgress, void *ctx) {
  LOG_DBG("FLASH",
          "[SIM] Firmware flashing is not supported in the native simulator");
  if (onProgress)
    onProgress(1, 1, ctx);
  return Result::WRITE_FAIL;
}

uint16_t runningPartitionChipId() { return 0xFFFF; }

"""


def patch(path: Path, marker: str, transform) -> bool:
    """Apply ``transform`` unless ``marker`` is already present. True = changed."""
    text = path.read_text()
    if marker in text:
        return False
    updated = transform(text)
    if updated == text:
        raise SystemExit(f"anchor not found in {path}; upstream layout changed, "
                         "update this patcher")
    path.write_text(updated)
    return True


def patch_env(env: str) -> None:
    src = ROOT / ".pio" / "libdeps" / env / "simulator" / "src"
    if not src.is_dir():
        raise SystemExit(f"{src} missing; run `pio pkg install -e {env}` first")

    changed = []

    # 1. HalClock: the firmware's status bar and BookOrbit sync ask the wall
    #    clock through these; the host clock is always available.
    if patch(src / "HalClock.h", "syncSystemTimeFromNTP", lambda t: t.replace(
            "#include <cstdint>",
            "#include <cstdint>\n#include <ctime>", 1).replace(
            "  bool syncFromNTP();\n",
            "  bool syncFromNTP();\n" + HALCLOCK_METHODS, 1)):
        changed.append("HalClock methods")

    # 2. startDeepSleep grew a keepPowerLatched flag (battery-latch boards);
    #    meaningless on a host, accepted and ignored.
    if patch(src / "HalPowerManager.h", "keepPowerLatched", lambda t: t.replace(
            "void startDeepSleep(HalGPIO &gpio) const;",
            "void startDeepSleep(HalGPIO &gpio, bool keepPowerLatched = false) const;", 1)):
        patch(src / "HalPowerManager.cpp", "startDeepSleep(HalGPIO &gpio, bool", lambda t: t.replace(
            "void HalPowerManager::startDeepSleep(HalGPIO &gpio) const {",
            "void HalPowerManager::startDeepSleep(HalGPIO &gpio, bool) const {", 1))
        changed.append("startDeepSleep(keepPowerLatched)")

    # 3. OTA flashing entry points the firmware links against; the simulator
    #    only ever reports "not supported".
    if patch(src / "simulator_firmware.cpp", "validateOpenImageFile", lambda t: t.replace(
            "namespace firmware_flash {",
            "class HalFile;\n\nnamespace firmware_flash {", 1).replace(
            "const char *resultName(Result r) {",
            FIRMWARE_FLASH_STUBS + "const char *resultName(Result r) {", 1)):
        changed.append("firmware_flash stubs")

    # 4. The firmware starts Wi-Fi scans asynchronously and treats anything but
    #    WIFI_SCAN_RUNNING (-1) as a failed start; the results are then read
    #    from scanComplete(), which already reports the fake networks.
    if patch(src / "WiFi.h", "return -1; // WIFI_SCAN_RUNNING", lambda t: t.replace(
            "    (void)async;\n    (void)show_hidden;",
            "    if (async)\n      return -1; // WIFI_SCAN_RUNNING\n    (void)show_hidden;", 1)):
        changed.append("async scanNetworks")

    if changed:
        print(f"[{env}] patched: {', '.join(changed)}")
    else:
        print(f"[{env}] package already carries every shim (upstream caught up?)")


def main() -> int:
    for env in sys.argv[1:] or ["simulator"]:
        patch_env(env)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
