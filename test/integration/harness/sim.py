"""The simulated device: one isolated SD card per scenario, the simulator process, and the
input that drives it.

The simulator runs headless in the card's parent folder, whose fs_/ is the SD card. A
synthetic Power press starts a sync, whose silent network reboot re-execs the process in
place. Synthetic input does not survive that reboot, so firmware-side hooks answer the
sync's screens and end the run (see BookOrbitSyncActivity's SIMULATOR blocks).
"""

from __future__ import annotations

import contextlib
import json
import os
import shutil
import socket
import struct
import subprocess
import tempfile
import time
import urllib.error
import urllib.request
from pathlib import Path

from formats import (obfuscate, read_download_index, read_list_caches, write_boa_store, write_clipping_store,
                     write_download_index, write_progress_bin)
from kosync import BASE_URL, partial_md5

ROOT = Path(__file__).resolve().parents[3]
INTEGRATION = ROOT / "test" / "integration"
LIBRARY = INTEGRATION / "library"
PROGRAM = ROOT / ".pio" / "build" / "simulator" / "program"

# The simulator's fixed fake MAC (simulator package esp_mac.h) drives both the
# credential obfuscation and the device id the firmware reports to the server.
SIM_MAC = bytes([0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x01])
SIM_DEVICE_ID = "crossink-" + SIM_MAC.hex()

SHORT_PWRBTN_SLEEP = 1  # CrossPointSettings::SHORT_PWRBTN::SLEEP
SHORT_PWRBTN_BOOKORBIT_SYNC = 37  # CrossPointSettings::SHORT_PWRBTN::BOOKORBIT_SYNC
SYNC_BEHAVIOR_SMART = 1
UI_THEME_BOOKORBIT = 8
BOOKORBIT_SYNC_FAILED = 7  # BookOrbitSyncActivity::SYNC_FAILED
SYNC_MARKER_MAGIC = 0x424F5359  # "BOSY", BookOrbitSmartSync::MARKER_MAGIC
SYNC_MARKER_FILE = "bookorbit_sync.bin"
ANNOTATION_STORE = "bookorbit_annotations.bin"
BOOKMARK_STORE = "bookorbit_bookmarks.bin"

CRASH_PATTERNS = ("Assertion failed", "Segmentation fault", "AddressSanitizer",
                  "UndefinedBehaviorSanitizer", "std::bad_alloc")

VERBOSE = False  # run_scenarios.py -v: print every run's simulator output


class SimFs:
    """One scenario's isolated SD card, as a device paired with the seeded account."""

    def __init__(self, temp_root: Path, kosync_creds: dict, download_folder: str = ""):
        self.root = temp_root
        self.fs = temp_root / "fs_"
        self.crosspoint = self.fs / ".crosspoint"
        self.crosspoint.mkdir(parents=True)
        (self.fs / "books").mkdir()
        self.download_folder = download_folder
        self.download_index = self.crosspoint / "bookorbit_downloads.bin"

        (self.crosspoint / "bookorbit.json").write_text(json.dumps({
            "username": kosync_creds["username"],
            "password_obf": obfuscate(kosync_creds["password"], SIM_MAC),
            "serverUrl": BASE_URL,
            "syncBehavior": SYNC_BEHAVIOR_SMART,
            "downloadFolder": download_folder,
        }))
        self.write_settings(shortPwrBtn=SHORT_PWRBTN_BOOKORBIT_SYNC)
        # A saved network lets the minimal network boot connect on its own (the
        # sim WiFi stub accepts anything); without one the sync parks on the
        # Wi-Fi selection screen forever. Plain "password" is the accepted
        # legacy field, sparing the harness the obfuscated variant.
        (self.crosspoint / "wifi.json").write_text(json.dumps({
            "lastConnectedSsid": "SimNet",
            "credentials": [{"ssid": "SimNet", "password": "simnet"}],
        }))

    def path(self, sd_path: str) -> Path:
        """The host file behind an SD path."""
        return self.fs / sd_path.lstrip("/")

    # --- settings ------------------------------------------------------------------

    def _update_json(self, name: str, values: dict) -> None:
        path = self.crosspoint / name
        data = json.loads(path.read_text())
        data.update(values)
        path.write_text(json.dumps(data))

    def write_settings(self, **values) -> None:
        """Replaces the device settings: what is not given takes its default."""
        (self.crosspoint / "crossink-settings.json").write_text(json.dumps(values))

    def update_settings(self, **values) -> None:
        self._update_json("crossink-settings.json", values)

    def update_bookorbit(self, **values) -> None:
        self._update_json("bookorbit.json", values)

    def enable_sync_on_sleep(self) -> None:
        """Sync on Sleep on, and a short Power press that puts the device to sleep."""
        self.update_bookorbit(syncOnSleep=True)
        self.write_settings(shortPwrBtn=SHORT_PWRBTN_SLEEP)

    def write_sync_marker(self, book_hash: str, timestamp: int,
                          position: tuple[int, int, int] | None = None) -> None:
        """Smart sync's memory of this device's last sync of the book: the server
        timestamp it saw and, unless written by a release that did not record it,
        the position both sides were left at (spine, page, page count)."""
        state_dir = self.state_dir(book_hash)
        state_dir.mkdir(parents=True, exist_ok=True)
        data = struct.pack("<Iq", SYNC_MARKER_MAGIC, timestamp)
        if position is not None:
            data += struct.pack("<HHH", *position)
        (state_dir / SYNC_MARKER_FILE).write_bytes(data)

    def read_sync_marker(self, book_hash: str) -> dict | None:
        path = self.state_dir(book_hash) / SYNC_MARKER_FILE
        raw = path.read_bytes() if path.exists() else b""
        if len(raw) < 12 or struct.unpack_from("<I", raw)[0] != SYNC_MARKER_MAGIC:
            return None
        marker: dict = {"timestamp": struct.unpack_from("<q", raw, 4)[0], "position": None}
        if len(raw) >= 18:
            marker["position"] = struct.unpack_from("<HHH", raw, 12)
        return marker

    # --- books ---------------------------------------------------------------------

    def place(self, source: Path, sd_path: str) -> Path:
        """Copies a file to an SD path, folders included."""
        dest = self.path(sd_path)
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, dest)
        return dest

    def open_book(self, book: dict, progress: tuple[int, int, int] | None = (0, 0, 10)) -> str:
        """Installs a library book under /books as the open book, at `progress` (spine,
        page, page count; the very beginning by default, none at all with None).
        Returns its SD path."""
        sim_path = f"/books/{Path(book['file']).name}"
        self.place(LIBRARY / book["file"], sim_path)
        self.set_open_book(sim_path)
        if progress is not None:
            write_progress_bin(self.progress_file(sim_path), *progress)
        return sim_path

    def set_open_book(self, sim_path: str) -> None:
        (self.crosspoint / "state.json").write_text(json.dumps({"openEpubPath": sim_path}))

    def set_asleep_in_book(self, sim_path: str) -> None:
        """As if the device went to sleep while reading: a run started with
        wake_into_reader resumes straight into the book, at its progress.bin."""
        (self.crosspoint / "state.json").write_text(json.dumps({
            "openEpubPath": sim_path, "lastSleepFromReader": True, "showBootScreen": False}))

    def write_local_highlights(self, book: dict, sim_path: str, highlights: list[tuple[dict, dict]],
                               watermark: int = 0, book_hash: str | None = None) -> None:
        """Highlights made on the device (formats.local_highlight), in the clipping store and
        the position records of the book's hash, or of another copy's."""
        book_hash = book_hash or book["hash"]
        write_clipping_store(self.clippings_store(book_hash), book.get("title", ""), book.get("author", ""),
                             sim_path, [clipping for clipping, _ in highlights])
        write_boa_store(self.annotation_store(book_hash), watermark, [record for _, record in highlights])

    # --- per-book files ------------------------------------------------------------

    def epub_cache_dir(self, sim_path: str) -> Path:
        # Epub::cachePathForFilePath: epub_<FNV-1a 64-bit of the path, decimal>.
        h = 14695981039346656037
        for b in sim_path.encode():
            h = ((h ^ b) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
        return self.crosspoint / f"epub_{h}"

    def progress_file(self, sim_path: str) -> Path:
        return self.epub_cache_dir(sim_path) / "progress.bin"

    def book_state_dir(self, host_path: Path) -> Path:
        return self.crosspoint / f"book_{partial_md5(host_path)}"

    # The clipping/bookmark stores and the BookOrbit sync-state dir are all keyed by the
    # book's content hash — the "hash" field library.json already carries.
    def state_dir(self, book_hash: str) -> Path:
        return self.crosspoint / f"book_{book_hash}"

    def clippings_store(self, book_hash: str) -> Path:
        return self.crosspoint / "clippings" / f"epub_{book_hash}.bin"

    def bookmarks_store(self, book_hash: str) -> Path:
        return self.crosspoint / "bookmarks" / f"epub_{book_hash}.bin"

    def annotation_store(self, book_hash: str) -> Path:
        return self.state_dir(book_hash) / ANNOTATION_STORE

    def bookmark_sync_store(self, book_hash: str) -> Path:
        return self.state_dir(book_hash) / BOOKMARK_STORE

    # --- catalog -------------------------------------------------------------------

    def list_caches(self) -> list[dict]:
        return read_list_caches(self.crosspoint / "bookorbit_lists")

    def read_download_index(self) -> list[tuple[int, str, int]]:
        return read_download_index(self.download_index, BASE_URL)

    def write_download_index(self, entries: list[tuple[int, str, int]], version: int) -> None:
        write_download_index(self.download_index, BASE_URL, entries, version)


@contextlib.contextmanager
def sd_card(manifest: dict, download_folder: str = ""):
    """A fresh card for the seeded account, gone with the scenario."""
    with tempfile.TemporaryDirectory(prefix="crossink-integ-") as tmp:
        yield SimFs(Path(tmp), manifest["kosync"], download_folder)


# --- runs --------------------------------------------------------------------------


def run_simulator(fs: SimFs, input_script: str, choice: str, timeout_s: int,
                  wake_into_reader: bool = False, after_reboot_script: str | None = None,
                  extra_env: dict[str, str] | None = None) -> str:
    env = os.environ.copy()
    env.update(extra_env or {})
    env.setdefault("SDL_VIDEODRIVER", "dummy")
    env["CROSSPOINT_SIM_INPUT_SCRIPT"] = input_script
    if after_reboot_script is not None:
        # The simulator drops the script when the firmware restarts; this one drives
        # the process the first restart launches, and only that one.
        env["CROSSPOINT_SIM_INPUT_SCRIPT_AFTER_REBOOT"] = after_reboot_script
    if wake_into_reader:
        # A power-button wake: with set_asleep_in_book, boot resumes the reader.
        env["CROSSPOINT_SIM_WAKE_REASON"] = "power"
    # Firmware-side scenario hooks (see BookOrbitSyncActivity's SIMULATOR
    # blocks): they answer the sync's screens and end the run, because
    # synthetic input does not survive the silent network reboot.
    env["CROSSINK_SIM_BOOKORBIT_CHOICE"] = choice
    env["CROSSINK_SIM_BOOKORBIT_QUIT_AFTER_SYNC"] = "1"
    # Stream to a file: the output survives a timeout kill, and a forked curl
    # inheriting a PIPE cannot wedge the harness on read.
    log_path = fs.root / "simulator.log"
    signal_number = 0
    with log_path.open("w") as log:
        try:
            result = subprocess.run([str(PROGRAM)], cwd=fs.root, env=env,
                                    stdout=log, stderr=subprocess.STDOUT, timeout=timeout_s)
            signal_number = -result.returncode if result.returncode < 0 else 0
        except subprocess.TimeoutExpired:
            # Scenarios end via a scripted QUIT; a timeout is survivable as long
            # as the sync had time to finish — the assertions decide.
            subprocess.run(["pkill", "-9", "-f", str(PROGRAM)], check=False)
    output = log_path.read_text(errors="replace")
    if VERBOSE:
        print(output)
    # A signal leaves no text behind ("Segmentation fault" is the shell's), and the
    # scenario's own assertions would then blame whatever the run did not reach.
    if signal_number:
        last = output.rstrip().splitlines()[-1:] or ["(no output)"]
        raise AssertionError(f"the simulator died on signal {signal_number} after: {last[0][:160]}")
    for pattern in CRASH_PATTERNS:
        if pattern in output:
            raise AssertionError(f"crash pattern in simulator output: {pattern}")
    return output


# The home screen, or the reader, is up well before 6 s.
POWER_PRESS = "6000:POWER:120"


def sync(fs: SimFs, choice: str, timeout_s: int = 90, what: str = "sync") -> str:
    """A short Power press on the seeded shortcut, BookOrbit Sync. `choice` answers the
    Apply/Upload screen Smart sync shows when it cannot decide alone, and the run ends
    once the sync settles."""
    output = run_simulator(fs, POWER_PRESS, choice=choice, timeout_s=timeout_s)
    assert "sync scenario finished" in output, f"{what} never reached its end marker"
    return output


def sleep_sync(fs: SimFs, timeout_s: int = 90) -> str:
    """A short Power press with Sync on Sleep on: the device goes to sleep and restarts into
    the sleep sync, which runs behind the sleep screen."""
    output = run_simulator(fs, POWER_PRESS, choice="none", timeout_s=timeout_s)
    assert "Sleep sync over" in output, "sleep sync never ran to its end"
    return output


def presses(start_ms: int, key: str, count: int, step_ms: int = 350) -> list[str]:
    """Input script steps: `count` presses of `key`, the first at `start_ms`."""
    return [f"{start_ms + i * step_ms}:{key}" for i in range(count)]


# Home menu -> BookOrbit catalog, from a boot with no open book.
TO_CATALOG = ["6000:DOWN", "6500:DOWN", "7000:CONFIRM"]
# The catalog's root, left once built: it counts the books on the card as it builds.
CATALOG_ROOT = ";".join(TO_CATALOG + ["13000:QUIT"])
# The BookOrbit Home's tile, the page's second stop without books: Confirm syncs the Home,
# over two restarts, so such a run can only end on its timeout.
HOME_TILE_SYNC = "3000:DOWN:120;4500:CONFIRM:120"


# --- the file transfer portal ------------------------------------------------------


def free_portal_port() -> int:
    """A port for the portal's HTTP server whose next one, the WebSocket server's, is free too."""
    with contextlib.closing(socket.socket()) as http, contextlib.closing(socket.socket()) as websocket:
        http.bind(("127.0.0.1", 0))
        port = http.getsockname()[1]
        websocket.bind(("127.0.0.1", port + 1))
        return port


@contextlib.contextmanager
def file_transfer_portal(fs: SimFs):
    """The simulator booted straight into the file transfer screen's hotspot mode (boot target
    6, payload 2), its web portal and WebDAV on a local port. Yields the portal's base URL."""
    port = free_portal_port()
    env = os.environ.copy()
    env.update(SDL_VIDEODRIVER="dummy", CROSSPOINT_SIM_SD=str(fs.fs), CROSSPOINT_SIM_HTTP_PORT=str(port),
               CROSSPOINT_SIM_SILENT_REBOOT_MAGIC=str(0xC1EAB007), CROSSPOINT_SIM_SILENT_REBOOT_TARGET="6",
               CROSSPOINT_SIM_SILENT_REBOOT_PAYLOAD="2")
    for name in ("CROSSPOINT_SIM_INPUT_SCRIPT", "CROSSINK_SIMULATOR_SMOKE_TEST",
                 "CROSSINK_SIM_BOOKORBIT_CHOICE", "CROSSINK_SIM_BOOKORBIT_QUIT_AFTER_SYNC"):
        env.pop(name, None)
    url = f"http://127.0.0.1:{port}"
    log_path = fs.root / "simulator.log"
    with log_path.open("w") as log:
        process = subprocess.Popen([str(PROGRAM)], cwd=fs.root, env=env, stdout=log, stderr=subprocess.STDOUT)
        try:
            deadline = time.monotonic() + 45
            while True:
                try:
                    with urllib.request.urlopen(url + "/", timeout=1) as response:
                        if response.status == 200:
                            break
                except (OSError, urllib.error.URLError):
                    assert process.poll() is None and time.monotonic() < deadline, \
                        "the simulator's web portal never started"
                    time.sleep(0.1)
            yield url
        finally:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
            if VERBOSE:
                print(log_path.read_text(errors="replace"))


def portal_request(method: str, url: str, data: bytes | None = None,
                   headers: dict[str, str] | None = None) -> tuple[int, str]:
    request = urllib.request.Request(url, data=data, headers=headers or {}, method=method)
    try:
        response = urllib.request.urlopen(request, timeout=20)
    except urllib.error.HTTPError as error:
        response = error
    with response:
        return response.getcode(), response.read().decode()
