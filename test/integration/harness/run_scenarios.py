#!/usr/bin/env python3
"""Run BookOrbit integration scenarios against the simulator.

Each scenario gets an isolated fs_/ SD directory, pre-written with BookOrbit
credentials (server URL, obfuscated password), settings (power shortcut =
BookOrbit Sync, resume state pointing at the scenario's book) and the book
itself. The simulator is launched headless; a synthetic power press triggers
the sync, the silent network reboot re-execs the process in place, and the
scenario then asserts on the SD tree and on the server via API.

Prerequisites: `docker compose -f ../docker-compose.yml up -d`, `seed/seed.py`
ran green, and `pio run -e simulator` built the binary.
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
INTEGRATION = ROOT / "test" / "integration"
sys.path.insert(0, str(INTEGRATION / "seed"))
from kosync import BASE_URL, KosyncDevice, partial_md5  # noqa: E402

PROGRAM = ROOT / ".pio" / "build" / "simulator" / "program"

# The simulator's fixed fake MAC (simulator package esp_mac.h) drives both the
# credential obfuscation and the device id the firmware reports to the server.
SIM_MAC = bytes([0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x01])
SIM_DEVICE_ID = "crossink-" + SIM_MAC.hex()

SHORT_PWRBTN_BOOKORBIT_SYNC = 33  # CrossPointSettings::SHORT_PWRBTN::BOOKORBIT_SYNC
SYNC_BEHAVIOR_SMART = 1

CRASH_PATTERNS = ("Assertion failed", "Segmentation fault", "AddressSanitizer",
                  "UndefinedBehaviorSanitizer", "std::bad_alloc")


# --- fs_ preparation ------------------------------------------------------------


def obfuscate_to_base64(plaintext: str) -> str:
    """Port of obfuscation::obfuscateToBase64 for the simulator's fixed MAC:
    "CPV1" + FNV1a32(MAC || plaintext) LE + plaintext, XOR MAC, base64."""
    h = 2166136261
    for b in SIM_MAC + plaintext.encode():
        h = ((h ^ b) * 16777619) & 0xFFFFFFFF
    payload = b"CPV1" + struct.pack("<I", h) + plaintext.encode()
    xored = bytes(c ^ SIM_MAC[i % len(SIM_MAC)] for i, c in enumerate(payload))
    return base64.b64encode(xored).decode()


def write_progress_bin(path: Path, spine: int, page: int, page_count: int,
                       visible_text_offset: int | None = None) -> None:
    """The reader's 6/10-byte progress format (see EpubReaderUtils.h)."""
    data = struct.pack("<HHH", spine, page, page_count)
    if visible_text_offset is not None:
        data += struct.pack("<I", visible_text_offset)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)


def read_progress_bin(path: Path) -> dict | None:
    if not path.exists():
        return None
    raw = path.read_bytes()
    if len(raw) not in (4, 6, 10):
        return None
    spine, page = struct.unpack_from("<HH", raw)
    out = {"spine": spine, "page": page}
    if len(raw) >= 6:
        out["pageCount"] = struct.unpack_from("<H", raw, 4)[0]
    if len(raw) == 10:
        out["visibleTextOffset"] = struct.unpack_from("<I", raw, 6)[0]
    return out


class SimFs:
    """One scenario's isolated SD card."""

    def __init__(self, temp_root: Path, kosync_creds: dict):
        self.root = temp_root
        self.fs = temp_root / "fs_"
        self.crosspoint = self.fs / ".crosspoint"
        self.crosspoint.mkdir(parents=True)
        (self.fs / "books").mkdir()

        (self.crosspoint / "bookorbit.json").write_text(json.dumps({
            "username": kosync_creds["username"],
            "password_obf": obfuscate_to_base64(kosync_creds["password"]),
            "serverUrl": BASE_URL,
            "syncBehavior": SYNC_BEHAVIOR_SMART,
        }))
        (self.crosspoint / "crossink-settings.json").write_text(json.dumps({
            "shortPwrBtn": SHORT_PWRBTN_BOOKORBIT_SYNC,
        }))
        # A saved network lets the minimal network boot connect on its own (the
        # sim WiFi stub accepts anything); without one the sync parks on the
        # Wi-Fi selection screen forever. Plain "password" is the accepted
        # legacy field, sparing the harness the obfuscated variant.
        (self.crosspoint / "wifi.json").write_text(json.dumps({
            "lastConnectedSsid": "SimNet",
            "credentials": [{"ssid": "SimNet", "password": "simnet"}],
        }))

    def add_book(self, source: Path) -> str:
        dest = self.fs / "books" / source.name
        shutil.copy2(source, dest)
        return f"/books/{source.name}"

    def set_open_book(self, sim_path: str) -> None:
        (self.crosspoint / "state.json").write_text(json.dumps({"openEpubPath": sim_path}))

    def epub_cache_dir(self, sim_path: str) -> Path:
        # Epub::cachePathForFilePath: epub_<FNV-1a 64-bit of the path, decimal>.
        h = 14695981039346656037
        for b in sim_path.encode():
            h = ((h ^ b) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
        return self.crosspoint / f"epub_{h}"

    def book_state_dir(self, host_path: Path) -> Path:
        return self.crosspoint / f"book_{partial_md5(host_path)}"


def run_simulator(fs: SimFs, input_script: str, choice: str, timeout_s: int,
                  verbose: bool) -> str:
    env = os.environ.copy()
    env.setdefault("SDL_VIDEODRIVER", "dummy")
    env["CROSSPOINT_SIM_INPUT_SCRIPT"] = input_script
    # Firmware-side scenario hooks (see BookOrbitSyncActivity's SIMULATOR
    # blocks): they answer the sync's screens and end the run, because
    # synthetic input does not survive the silent network reboot.
    env["CROSSINK_SIM_BOOKORBIT_CHOICE"] = choice
    env["CROSSINK_SIM_BOOKORBIT_QUIT_AFTER_SYNC"] = "1"
    # Stream to a file: the output survives a timeout kill, and a forked curl
    # inheriting a PIPE cannot wedge the harness on read.
    log_path = fs.root / "simulator.log"
    with log_path.open("w") as log:
        try:
            subprocess.run([str(PROGRAM)], cwd=fs.root, env=env,
                           stdout=log, stderr=subprocess.STDOUT, timeout=timeout_s)
        except subprocess.TimeoutExpired:
            # Scenarios end via a scripted QUIT; a timeout is survivable as long
            # as the sync had time to finish — the assertions decide.
            subprocess.run(["pkill", "-9", "-f", str(PROGRAM)], check=False)
    output = log_path.read_text(errors="replace")
    if verbose:
        print(output)
    for pattern in CRASH_PATTERNS:
        if pattern in output:
            raise AssertionError(f"crash pattern in simulator output: {pattern}")
    return output


# --- scenarios ------------------------------------------------------------------


def load_seed() -> tuple[dict, dict]:
    manifest = json.loads((INTEGRATION / "seed-manifest.json").read_text())
    library = {b["hash"]: b for b in json.loads((INTEGRATION / "library.json").read_text())}
    return manifest, library


def scenario_sync_progress_pull(verbose: bool) -> None:
    """A further server-side position (seeded by the synthetic peer) reaches the
    simulator through a Smart sync triggered from the home screen."""
    manifest, _ = load_seed()
    seeded = manifest["progress"][0]
    source = INTEGRATION / "library" / seeded["file"]

    with tempfile.TemporaryDirectory(prefix="crossink-integ-") as tmp:
        fs = SimFs(Path(tmp), manifest["kosync"])
        sim_path = fs.add_book(source)
        fs.set_open_book(sim_path)
        # Local progress: the very beginning, clearly behind the seeded 40%.
        write_progress_bin(fs.epub_cache_dir(sim_path) / "progress.bin", 0, 0, 10)

        # Home screen is up well before 6s; POWER short-press starts the sync,
        # which silently reboots into it. A first sync of a book has no history,
        # so Smart sync shows the Apply/Upload choice; the firmware hook answers
        # it and ends the process once the sync settles.
        output = run_simulator(fs, input_script="6000:POWER:120", choice="apply",
                               timeout_s=90, verbose=verbose)
        assert "sync scenario finished" in output, "sync never reached its end marker"

        for home, progress in (("path-keyed", read_progress_bin(fs.epub_cache_dir(sim_path) / "progress.bin")),
                               ("content-keyed", read_progress_bin(fs.book_state_dir(source) / "progress.bin"))):
            assert progress is not None, f"{home} progress.bin missing after sync"
            assert progress["spine"] > 0, f"{home} progress did not advance: {progress}"


def scenario_sync_progress_push(verbose: bool) -> None:
    """A further local position reaches the server under the simulator's device id."""
    manifest, _ = load_seed()
    seeded = manifest["progress"][1]
    source = INTEGRATION / "library" / seeded["file"]
    book_hash = seeded["hash"]

    with tempfile.TemporaryDirectory(prefix="crossink-integ-") as tmp:
        fs = SimFs(Path(tmp), manifest["kosync"])
        sim_path = fs.add_book(source)
        fs.set_open_book(sim_path)
        # Local progress in the last chapter (the fixture has 5 spine items;
        # an out-of-range spine would be clamped back to 0): clearly ahead of
        # the seeded 40%.
        write_progress_bin(fs.epub_cache_dir(sim_path) / "progress.bin", 4, 0, 1)

        # Same flow, answering "Upload" instead.
        output = run_simulator(fs, input_script="6000:POWER:120", choice="upload",
                               timeout_s=90, verbose=verbose)
        assert "sync scenario finished" in output, "sync never reached its end marker"

    peer = KosyncDevice(BASE_URL, manifest["kosync"]["username"],
                        manifest["kosync"]["password"], manifest["peer_device_id"])
    remote = peer.get_progress(book_hash)
    assert float(remote.get("percentage", 0)) > 0.5, f"server progress not advanced: {remote}"
    assert remote.get("device_id") == SIM_DEVICE_ID, f"unexpected device id: {remote}"


SCENARIOS = {
    "sync_progress_pull": scenario_sync_progress_pull,
    "sync_progress_push": scenario_sync_progress_push,
}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--scenario", choices=sorted(SCENARIOS), help="run one scenario only")
    parser.add_argument("-v", "--verbose", action="store_true", help="print simulator output")
    args = parser.parse_args()

    # A previous aborted run can leave an orphaned simulator alive, holding
    # stale state against the same server.
    subprocess.run(["pkill", "-9", "-f", str(PROGRAM)], check=False)

    if not PROGRAM.exists():
        print(f"Simulator binary missing: {PROGRAM}\nRun: pio run -e simulator", file=sys.stderr)
        return 2

    names = [args.scenario] if args.scenario else sorted(SCENARIOS)
    failures = 0
    for name in names:
        start = time.monotonic()
        try:
            SCENARIOS[name](args.verbose)
            print(f"PASS {name} ({time.monotonic() - start:.1f}s)")
        except AssertionError as e:
            failures += 1
            print(f"FAIL {name}: {e}", file=sys.stderr)
        except Exception as e:  # infrastructure error, not a test verdict
            failures += 1
            print(f"ERROR {name}: {type(e).__name__}: {e}", file=sys.stderr)
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
