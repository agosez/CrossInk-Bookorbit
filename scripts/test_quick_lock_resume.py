#!/usr/bin/env python3
"""Check that a Quick Lock timeout wake resumes the reader unlocked."""

import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def check_resume(program: Path, suffix: str, activity: str, orientation: int) -> None:
    with tempfile.TemporaryDirectory(prefix="crossink-quick-lock-") as directory:
        work = Path(directory)
        state_dir = work / "fs_" / ".crosspoint"
        state_dir.mkdir(parents=True)
        book = work / "fs_" / f"book.{suffix}"
        if suffix == "epub":
            shutil.copy2(ROOT / "test/epubs/test_reader_rendering_matrix.epub", book)
        else:
            book.write_text("Quick Lock wake regression.\n" * 80)
        (state_dir / "crossink-settings.json").write_text(json.dumps({"orientation": orientation}))
        (state_dir / "state.json").write_text(json.dumps({
            "openEpubPath": f"/book.{suffix}",
            "lastSleepFromReader": True,
            "showBootScreen": False,
            "quickLockResumePending": True,
            "quickLockRestoreFrontlight": True,
        }))
        env = os.environ.copy()
        for key in list(env):
            if key.startswith(("CROSSPOINT_SIM_", "CROSSINK_SIMULATOR_SMOKE")):
                del env[key]
        env.update(SDL_VIDEODRIVER="dummy", CROSSPOINT_SIM_WAKE_REASON="power",
                   CROSSPOINT_SIM_INPUT_SCRIPT="2000:QUIT")
        result = subprocess.run([str(program)], cwd=work, env=env, capture_output=True, text=True, timeout=20)
        output = result.stdout + result.stderr
        entered = output.find(f"Entering activity: {activity}")
        if result.returncode or entered < 0 or "Quick Lock enabled" in output:
            raise AssertionError(f"{suffix} orientation={orientation}: Quick Lock remained active after wake\n{output}")
        state = json.loads((state_dir / "state.json").read_text())
        if (state.get("openEpubPath") != f"/book.{suffix}" or state.get("quickLockResumePending", False)
                or state.get("quickLockRestoreFrontlight", False)):
            raise AssertionError(f"{suffix}: wake did not clear transient Quick Lock state: {state}")
        print(f"PASS: {suffix} orientation={orientation} resumes without Quick Lock")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--env", choices=("simulator", "x4-pro-simulator", "sticky-simulator"), default="simulator")
    args = parser.parse_args()
    program = ROOT / ".pio/build" / args.env / "program"
    for orientation in (0, 3):
        for suffix, activity in (("epub", "EpubReader"), ("txt", "TxtReader")):
            check_resume(program, suffix, activity, orientation)


if __name__ == "__main__":
    main()
