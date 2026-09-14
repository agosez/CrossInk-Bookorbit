#!/usr/bin/env python3
"""Check landscape reader-pane transitions for stale drawer outlines."""

import argparse
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def rule_groups(path: Path) -> int:
    data = path.read_bytes()
    offset = struct.unpack_from("<I", data, 10)[0]
    width, height = struct.unpack_from("<ii", data, 18)
    bits = struct.unpack_from("<H", data, 28)[0]
    if data[:2] != b"BM" or bits not in (24, 32) or width <= 0 or height == 0:
        raise AssertionError("Unsupported simulator screenshot")
    pixel_bytes = bits // 8
    stride = ((width * pixel_bytes + 3) // 4) * 4
    groups = 0
    previous = False
    # Exclude the tab-bar separator at the bottom. Drawer rules span the
    # screen, whereas preview text and the grabber occupy only part of a row.
    for y in range(abs(height) * 3 // 4):
        source_y = height - 1 - y if height > 0 else y
        row = data[offset + source_y * stride:offset + (source_y + 1) * stride]
        dark = sum(max(row[x:x + 3]) < 64 for x in range(0, width * pixel_bytes, pixel_bytes))
        is_rule = dark > width * 0.9
        groups += is_rule and not previous
        previous = is_rule
    return groups


def check_drawer(program: Path, orientation: int) -> None:
    with tempfile.TemporaryDirectory(prefix="crossink-drawer-") as directory:
        work = Path(directory)
        state = work / "fs_/.crosspoint"
        state.mkdir(parents=True)
        shutil.copy2(ROOT / "test/epubs/test_reader_rendering_matrix.epub", work / "fs_/book.epub")
        (state / "crossink-settings.json").write_text(json.dumps({"orientation": orientation}))
        (state / "state.json").write_text(json.dumps({
            "openEpubPath": "/book.epub", "lastSleepFromReader": True, "showBootScreen": False,
        }))
        screenshot = work / "drawer.bmp"
        env = os.environ.copy()
        for key in list(env):
            if key.startswith(("CROSSPOINT_SIM_", "CROSSINK_SIMULATOR_SMOKE")):
                del env[key]
        # Normalized positions remain valid when boot starts in portrait and
        # the reader subsequently restores a landscape orientation.
        env.update(SDL_VIDEODRIVER="dummy", CROSSPOINT_SIM_WAKE_REASON="power",
                   CROSSPOINT_SIM_INPUT_SCRIPT=("2000:SWIPE:0.5,0.8,0.5,0.4,250;"
                                                "4000:TAP:0.5,0.4,80;6000:QUIT"),
                   CROSSPOINT_SIM_SCREENSHOTS=f"5000:{screenshot}")
        result = subprocess.run([str(program)], cwd=work, env=env, capture_output=True, text=True, timeout=20)
        output = result.stdout + result.stderr
        if result.returncode or "Entering activity: EpubReaderTouchMenu" not in output:
            raise AssertionError(f"Reader drawer did not open\n{output}")
        groups = rule_groups(screenshot)
        if groups != 1:
            raise AssertionError(f"orientation={orientation}: expected one drawer rule, found {groups}")
        print(f"PASS: orientation={orientation} has one drawer outline")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--env", choices=("x4-pro-simulator", "sticky-simulator"), default="x4-pro-simulator")
    args = parser.parse_args()
    program = ROOT / ".pio/build" / args.env / "program"
    for orientation in (1, 3):
        check_drawer(program, orientation)


if __name__ == "__main__":
    main()
