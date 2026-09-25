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
from kosync import BASE_URL, KosyncDevice, kodatetime, partial_md5  # noqa: E402

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


# --- reader-store serialization (formats mirror src/ClippingStore.cpp,
# src/BookmarkStore.cpp and lib/BookOrbitSync/BookOrbit*Store.cpp) ----------------

CHAPTER_TITLE_MAX = 48
SNIPPET_MAX = 64


def _pack_str(s: str) -> bytes:
    data = s.encode()
    return struct.pack("<I", len(data)) + data


def _read_str(raw: bytes, offset: int) -> tuple[str, int]:
    (length,) = struct.unpack_from("<I", raw, offset)
    offset += 4
    return raw[offset:offset + length].decode(errors="replace"), offset + length


def _fixed_str(s: str, size: int) -> bytes:
    return s.encode()[:size - 1].ljust(size, b"\0")


def write_clipping_store(path: Path, title: str, author: str, book_path: str,
                         clippings: list[dict]) -> None:
    """Clipping store v4: header then per record 8×u16, u32 timestamp,
    u32 layoutSignature, u16 tableSelection, char[48] chapter, u16 textLen, text."""
    blob = struct.pack("<BH", 4, len(clippings))
    blob += _pack_str(title) + _pack_str(author) + _pack_str(book_path)
    for c in clippings:
        text = c["text"].encode()
        blob += struct.pack("<8H", c["spine"], c.get("startPage", 0), c.get("endPage", 0),
                            c.get("pageCount", 0), 0, 0, 0, c.get("paragraph", 0xFFFF))
        blob += struct.pack("<II", c["timestamp"], c.get("layoutSignature", 0))
        blob += struct.pack("<H", c.get("tableSelection", 0xFFFF))  # UINT16_MAX: not a table cell
        blob += _fixed_str(c.get("chapter", ""), CHAPTER_TITLE_MAX)
        blob += struct.pack("<H", len(text)) + text
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(blob)


def read_clipping_store(path: Path) -> list[dict]:
    if not path.exists():
        return []
    raw = path.read_bytes()
    version, count = struct.unpack_from("<BH", raw, 0)
    assert version in (3, 4), f"unexpected clipping store version {version} in {path}"
    offset = 3
    title, offset = _read_str(raw, offset)
    author, offset = _read_str(raw, offset)
    book_path, offset = _read_str(raw, offset)
    out = []
    for _ in range(count):
        spine, _sp, _ep, _pc, _sw, _ew, _wc, paragraph = struct.unpack_from("<8H", raw, offset)
        offset += 16
        timestamp, _sig = struct.unpack_from("<II", raw, offset)
        offset += 8
        if version >= 4:
            offset += 2  # tableSelection (u16), irrelevant to the exchanged text
        chapter = raw[offset:offset + CHAPTER_TITLE_MAX].split(b"\0", 1)[0].decode(errors="replace")
        offset += CHAPTER_TITLE_MAX
        (text_len,) = struct.unpack_from("<H", raw, offset)
        offset += 2
        text = raw[offset:offset + text_len].decode(errors="replace")
        offset += text_len
        out.append({"spine": spine, "paragraph": paragraph, "timestamp": timestamp,
                    "chapter": chapter, "text": text})
    return out


def write_bookmark_store(path: Path, title: str, author: str, book_path: str,
                         bookmarks: list[dict]) -> None:
    """Bookmark store v5: header then per record u16 spine, f32 progress,
    u32 timestamp, char[48] chapter, u16 paragraph, char[64] snippet."""
    blob = struct.pack("<BH", 5, len(bookmarks))
    blob += _pack_str(title) + _pack_str(author) + _pack_str(book_path)
    for b in bookmarks:
        blob += struct.pack("<HfI", b["spine"], b.get("progress", 0.0), b["timestamp"])
        blob += _fixed_str(b.get("chapter", ""), CHAPTER_TITLE_MAX)
        blob += struct.pack("<H", b.get("paragraph", 0xFFFF))
        blob += _fixed_str(b.get("snippet", ""), SNIPPET_MAX)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(blob)


def read_bookmark_store(path: Path) -> list[dict]:
    if not path.exists():
        return []
    raw = path.read_bytes()
    version, count = struct.unpack_from("<BH", raw, 0)
    assert version == 5, f"unexpected bookmark store version {version} in {path}"
    offset = 3
    for _ in range(3):  # title, author, path
        _, offset = _read_str(raw, offset)
    out = []
    for _ in range(count):
        spine, progress, timestamp = struct.unpack_from("<HfI", raw, offset)
        offset += 10
        chapter = raw[offset:offset + CHAPTER_TITLE_MAX].split(b"\0", 1)[0].decode(errors="replace")
        offset += CHAPTER_TITLE_MAX
        (paragraph,) = struct.unpack_from("<H", raw, offset)
        offset += 2
        snippet = raw[offset:offset + SNIPPET_MAX].split(b"\0", 1)[0].decode(errors="replace")
        offset += SNIPPET_MAX
        out.append({"spine": spine, "progress": progress, "timestamp": timestamp,
                    "chapter": chapter, "paragraph": paragraph, "snippet": snippet})
    return out


def write_boa_store(path: Path, watermark: int, records: list[dict]) -> None:
    """BookOrbit annotation sync-state store ("BOA1")."""
    blob = b"BOA1" + struct.pack("<I", watermark)
    for r in records:
        pos0, pos1 = r["pos0"].encode(), r["pos1"].encode()
        blob += struct.pack("<IIHHHH", r["timestamp"], r["identityEpoch"], r["spine"],
                            r.get("paragraph", 0xFFFF), len(pos0), len(pos1))
        blob += pos0 + pos1
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(blob)


def read_boa_store(path: Path) -> tuple[int, list[dict]]:
    if not path.exists():
        return 0, []
    raw = path.read_bytes()
    assert raw[:4] == b"BOA1", f"unexpected annotation store magic in {path}"
    (watermark,) = struct.unpack_from("<I", raw, 4)
    offset, out = 8, []
    while offset < len(raw):
        ts, epoch, spine, paragraph, len0, len1 = struct.unpack_from("<IIHHHH", raw, offset)
        offset += 16
        pos0 = raw[offset:offset + len0].decode(errors="replace")
        offset += len0
        pos1 = raw[offset:offset + len1].decode(errors="replace")
        offset += len1
        out.append({"timestamp": ts, "identityEpoch": epoch, "spine": spine,
                    "paragraph": paragraph, "pos0": pos0, "pos1": pos1})
    return watermark, out


def write_bob_store(path: Path, watermark: int, records: list[dict]) -> None:
    """BookOrbit bookmark sync-state store ("BOB1")."""
    blob = b"BOB1" + struct.pack("<I", watermark)
    for r in records:
        pos = r["pos"].encode()
        blob += struct.pack("<IIHH", r["timestamp"], r["identityEpoch"], r["spine"], len(pos))
        blob += pos
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(blob)


def read_bob_store(path: Path) -> tuple[int, list[dict]]:
    if not path.exists():
        return 0, []
    raw = path.read_bytes()
    assert raw[:4] == b"BOB1", f"unexpected bookmark store magic in {path}"
    (watermark,) = struct.unpack_from("<I", raw, 4)
    offset, out = 8, []
    while offset < len(raw):
        ts, epoch, spine, pos_len = struct.unpack_from("<IIHH", raw, offset)
        offset += 12
        pos = raw[offset:offset + pos_len].decode(errors="replace")
        offset += pos_len
        out.append({"timestamp": ts, "identityEpoch": epoch, "spine": spine, "pos": pos})
    return watermark, out


class SimFs:
    """One scenario's isolated SD card."""

    def __init__(self, temp_root: Path, kosync_creds: dict, download_folder: str = ""):
        self.root = temp_root
        self.fs = temp_root / "fs_"
        self.crosspoint = self.fs / ".crosspoint"
        self.crosspoint.mkdir(parents=True)
        (self.fs / "books").mkdir()
        self.download_folder = download_folder

        (self.crosspoint / "bookorbit.json").write_text(json.dumps({
            "username": kosync_creds["username"],
            "password_obf": obfuscate_to_base64(kosync_creds["password"]),
            "serverUrl": BASE_URL,
            "syncBehavior": SYNC_BEHAVIOR_SMART,
            "downloadFolder": download_folder,
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

    # The clipping/bookmark stores and the BookOrbit sync-state dir are all keyed by the
    # book's content hash — the "hash" field library.json already carries.
    def state_dir(self, book_hash: str) -> Path:
        return self.crosspoint / f"book_{book_hash}"

    def clippings_store(self, book_hash: str) -> Path:
        return self.crosspoint / "clippings" / f"epub_{book_hash}.bin"

    def bookmarks_store(self, book_hash: str) -> Path:
        return self.crosspoint / "bookmarks" / f"epub_{book_hash}.bin"


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


def load_seed() -> tuple[dict, list[dict]]:
    manifest = json.loads((INTEGRATION / "seed-manifest.json").read_text())
    library = json.loads((INTEGRATION / "library.json").read_text())
    return manifest, library


# Library indices for scenarios that need a book with NO seeded server state,
# outside every seeded range in seed/seed.py (progress 0-9, highlights 10-19,
# bookmarks 20-24). One book per scenario so runs never contaminate each other.
FRESH_BOOK = {
    "highlight_push": 30,
    "bookmark_push": 31,
    "highlight_delete": 32,
    "highlight_delete_guard": 33,
    "bookmark_pull": 34,
}

# Bookmarks dedupe server-side on their converted location (paragraph precision), so a
# re-run "new" bookmark lands on the SAME server row. A row's identity key (datetime+pos)
# must therefore stay fixed across runs, exactly as a real device keeps a bookmark's
# minted datetime for life — re-minting one is what tombstones the row through the
# complete-key-set diff.
BOOKMARK_PUSH_EPOCH = 1_756_100_000

ANNOTATION_STORE = "bookorbit_annotations.bin"
BOOKMARK_STORE = "bookorbit_bookmarks.bin"


def peer_device(manifest: dict) -> KosyncDevice:
    """The synthetic 'other reader' that seeded the server-side state."""
    dev = KosyncDevice(BASE_URL, manifest["kosync"]["username"],
                       manifest["kosync"]["password"], manifest["peer_device_id"])
    dev.auth()
    return dev


def verify_device(manifest: dict, tag: str) -> KosyncDevice:
    """A never-seen device id: the server offers it everything it has no sync
    state for, which makes it an honest view of what exists server-side."""
    dev = KosyncDevice(BASE_URL, manifest["kosync"]["username"],
                       manifest["kosync"]["password"], f"crossink-verify-{tag}-{int(time.time())}")
    dev.auth()
    return dev


def annotation_adds(device: KosyncDevice, book_hash: str) -> list[dict]:
    resp = device.exchange_annotations(book_hash, keys=[], keys_complete=False, changes=[])
    assert not resp.get("unmatched"), f"server does not know book {book_hash}: {resp}"
    return resp["results"][0]["toApply"]["add"]


def bookmark_adds(device: KosyncDevice, book_hash: str) -> list[dict]:
    resp = device.exchange_bookmarks(book_hash, keys=[], keys_complete=False, changes=[])
    assert not resp.get("unmatched"), f"server does not know book {book_hash}: {resp}"
    return resp["results"][0]["toApply"]["add"]


def reset_sim_bookmarks(manifest: dict, book_hash: str) -> None:
    """Detach the simulator's fixed device id from every bookmark on this book, so the
    next peer bookmark is offered to it again (the server offers a bookmark only to
    devices holding no link for it). Impersonates the simulator: an empty-complete key
    set tombstones what it held and drops those links; acking the returned deletion
    offers drops any stale links on already-tombstoned rows."""
    sim = KosyncDevice(BASE_URL, manifest["kosync"]["username"],
                       manifest["kosync"]["password"], SIM_DEVICE_ID)
    sim.auth()
    resp = sim.exchange_bookmarks(book_hash, keys=[], keys_complete=True, changes=[])
    deletes = resp["results"][0]["toApply"]["delete"]
    if deletes:
        sim.exchange_bookmarks_ack(book_hash, deleted=[
            {"serverId": d["serverId"], "status": "applied"} for d in deletes])


def make_fs(tmp: str, manifest: dict, book: dict) -> tuple[SimFs, str, Path]:
    """The common setup: isolated SD, the book installed and open, local progress
    at the very beginning."""
    fs = SimFs(Path(tmp), manifest["kosync"])
    source = INTEGRATION / "library" / book["file"]
    sim_path = fs.add_book(source)
    fs.set_open_book(sim_path)
    write_progress_bin(fs.epub_cache_dir(sim_path) / "progress.bin", 0, 0, 10)
    return fs, sim_path, source


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


def scenario_highlight_pull(verbose: bool) -> None:
    """A highlight created by another device reaches the simulator's clipping
    store, and its position is recorded for future syncs. A fresh peer highlight
    per run keeps this repeatable: the simulator's device id has already
    acknowledged older ones on previous runs."""
    manifest, _ = load_seed()
    book = manifest["highlights"][0]
    now = int(time.time())
    text = f"Peer highlight {now}"
    peer = peer_device(manifest)
    peer.exchange_annotations(book["hash"], keys=[], keys_complete=False, changes=[{
        "datetime": kodatetime(now),
        "pos0": "/body/DocFragment[3]/body/p[2]/text().0",
        "pos1": f"/body/DocFragment[3]/body/p[2]/text().{len(text)}",
        "text": text,
        "chapter": "Chapter",
    }])

    with tempfile.TemporaryDirectory(prefix="crossink-integ-") as tmp:
        fs, _, _ = make_fs(tmp, manifest, book)
        output = run_simulator(fs, input_script="6000:POWER:120", choice="apply",
                               timeout_s=90, verbose=verbose)
        assert "sync scenario finished" in output, "sync never reached its end marker"

        clippings = read_clipping_store(fs.clippings_store(book["hash"]))
        mine = [c for c in clippings if c["text"] == text]
        assert mine, f"peer highlight missing locally; texts={[c['text'] for c in clippings]}"
        assert mine[0]["spine"] == 2, f"highlight landed in the wrong chapter: {mine[0]}"
        _, records = read_boa_store(fs.state_dir(book["hash"]) / ANNOTATION_STORE)
        assert any(r["pos0"].startswith("/body/DocFragment[3]") for r in records), \
            f"no position record for the received highlight: {records}"


def scenario_highlight_push(verbose: bool) -> None:
    """A local highlight (clipping + minted position record, exactly what the
    reader persists on creation) reaches the server and is offered to a device
    that has never seen it."""
    manifest, books = load_seed()
    book = books[FRESH_BOOK["highlight_push"]]
    now = int(time.time())
    text = f"Local highlight {now}"

    with tempfile.TemporaryDirectory(prefix="crossink-integ-") as tmp:
        fs, sim_path, _ = make_fs(tmp, manifest, book)
        # timestamp is the reader's millis()/1000 uptime stamp (any nonzero joins
        # the two stores); identityEpoch is the WallClock date the server validates.
        write_clipping_store(fs.clippings_store(book["hash"]), book.get("title", ""),
                             book.get("author", ""), sim_path,
                             [{"spine": 1, "paragraph": 2, "timestamp": 4242,
                               "text": text, "chapter": "Chapter"}])
        write_boa_store(fs.state_dir(book["hash"]) / ANNOTATION_STORE, 0, [{
            "timestamp": 4242, "identityEpoch": now, "spine": 1, "paragraph": 2,
            "pos0": "/body/DocFragment[2]/body/p[2]/text().0",
            "pos1": f"/body/DocFragment[2]/body/p[2]/text().{len(text)}",
        }])

        output = run_simulator(fs, input_script="6000:POWER:120", choice="upload",
                               timeout_s=90, verbose=verbose)
        assert "sync scenario finished" in output, "sync never reached its end marker"
        watermark, _ = read_boa_store(fs.state_dir(book["hash"]) / ANNOTATION_STORE)
        assert watermark >= now, f"upload watermark did not advance: {watermark} < {now}"

    adds = annotation_adds(verify_device(manifest, "hlpush"), book["hash"])
    assert any(a["text"] == text for a in adds), \
        f"pushed highlight not on server; offered texts={[a['text'] for a in adds]}"


def scenario_highlight_delete_propagates(verbose: bool) -> None:
    """Deleting a synced highlight locally deletes it on the server: the second
    sync's complete key set no longer names it. Two simulator runs on one SD."""
    manifest, books = load_seed()
    book = books[FRESH_BOOK["highlight_delete"]]
    now = int(time.time())
    text = f"Doomed highlight {now}"
    peer = peer_device(manifest)
    peer.exchange_annotations(book["hash"], keys=[], keys_complete=False, changes=[{
        "datetime": kodatetime(now),
        "pos0": "/body/DocFragment[3]/body/p[2]/text().0",
        "pos1": f"/body/DocFragment[3]/body/p[2]/text().{len(text)}",
        "text": text,
        "chapter": "Chapter",
    }])

    with tempfile.TemporaryDirectory(prefix="crossink-integ-") as tmp:
        fs, sim_path, _ = make_fs(tmp, manifest, book)
        output = run_simulator(fs, input_script="6000:POWER:120", choice="apply",
                               timeout_s=90, verbose=verbose)
        assert "sync scenario finished" in output, "first sync never finished"
        clippings = read_clipping_store(fs.clippings_store(book["hash"]))
        assert any(c["text"] == text for c in clippings), "setup: highlight never arrived"

        # The user deletes the highlight: the clipping goes, the position record
        # stays behind (the next sync drops the orphan and reports the shrunken set).
        write_clipping_store(fs.clippings_store(book["hash"]), "", "", sim_path,
                             [c for c in clippings if c["text"] != text])

        output = run_simulator(fs, input_script="6000:POWER:120", choice="apply",
                               timeout_s=90, verbose=verbose)
        assert "sync scenario finished" in output, "second sync never finished"

    adds = annotation_adds(verify_device(manifest, "hldel"), book["hash"])
    assert not any(a["text"] == text for a in adds), \
        f"deleted highlight still on server: {[a['text'] for a in adds]}"


def scenario_highlight_delete_guard(verbose: bool) -> None:
    """A lost sync-state store must NOT read as 'every highlight deleted': with
    the store gone, the sync reports no key set and the server keeps everything.
    This is the destructive-wipe guard in prepareAnnotationBatch."""
    manifest, books = load_seed()
    book = books[FRESH_BOOK["highlight_delete_guard"]]
    now = int(time.time())
    text = f"Survivor highlight {now}"
    peer = peer_device(manifest)
    peer.exchange_annotations(book["hash"], keys=[], keys_complete=False, changes=[{
        "datetime": kodatetime(now),
        "pos0": "/body/DocFragment[3]/body/p[2]/text().0",
        "pos1": f"/body/DocFragment[3]/body/p[2]/text().{len(text)}",
        "text": text,
        "chapter": "Chapter",
    }])

    with tempfile.TemporaryDirectory(prefix="crossink-integ-") as tmp:
        fs, sim_path, _ = make_fs(tmp, manifest, book)
        output = run_simulator(fs, input_script="6000:POWER:120", choice="apply",
                               timeout_s=90, verbose=verbose)
        assert "sync scenario finished" in output, "first sync never finished"
        clippings = read_clipping_store(fs.clippings_store(book["hash"]))
        assert any(c["text"] == text for c in clippings), "setup: highlight never arrived"

        # History lost (wiped card, fresh device) AND the clippings gone: without
        # the guard, the empty-complete key set would erase the server's copy.
        (fs.state_dir(book["hash"]) / ANNOTATION_STORE).unlink()
        write_clipping_store(fs.clippings_store(book["hash"]), "", "", sim_path, [])

        output = run_simulator(fs, input_script="6000:POWER:120", choice="apply",
                               timeout_s=90, verbose=verbose)
        assert "sync scenario finished" in output, "second sync never finished"
        assert "No highlight sync history" in output, \
            "guard log missing: deletions may have been reported over a lost store"

    adds = annotation_adds(verify_device(manifest, "hlguard"), book["hash"])
    assert any(a["text"] == text for a in adds), \
        "server highlight was erased despite the lost local store"


def scenario_bookmark_pull(verbose: bool) -> None:
    """A bookmark created by another device reaches the simulator's bookmark
    store, with a position record minted for future syncs. The simulator's fixed
    device id keeps its server-side link across runs, so the scenario first
    detaches it, then has the peer re-mint the bookmark (fresh datetime = fresh
    key, restoring the tombstoned row for everyone but the simulator)."""
    manifest, books = load_seed()
    book = books[FRESH_BOOK["bookmark_pull"]]
    now = int(time.time())
    pos = "/body/DocFragment[2]/body/p[1]/text().0"
    reset_sim_bookmarks(manifest, book["hash"])
    peer = peer_device(manifest)
    peer.exchange_bookmarks(book["hash"], keys=[], keys_complete=False,
                            changes=[{"datetime": kodatetime(now), "pos": pos}])

    with tempfile.TemporaryDirectory(prefix="crossink-integ-") as tmp:
        fs, _, _ = make_fs(tmp, manifest, book)
        output = run_simulator(fs, input_script="6000:POWER:120", choice="apply",
                               timeout_s=90, verbose=verbose)
        assert "sync scenario finished" in output, "sync never reached its end marker"

        _, records = read_bob_store(fs.state_dir(book["hash"]) / BOOKMARK_STORE)
        assert any(r["pos"] == pos for r in records), \
            f"no position record for the received bookmark: {records}"
        bookmarks = read_bookmark_store(fs.bookmarks_store(book["hash"]))
        assert any(b["spine"] == 1 for b in bookmarks), \
            f"bookmark did not land in chapter 2: {bookmarks}"


def scenario_catalog_collections_browse(verbose: bool) -> None:
    """The catalog browser lists the server's Collections section and the books
    inside one. Driven entirely by scripted input (the catalog flow never
    silent-reboots before exit), asserted through the list cache the browser
    writes for each screen it loaded. Browse-only: nothing mutates server-side,
    so this is repeatable as-is."""
    manifest, _ = load_seed()
    collection = manifest["collection"]

    with tempfile.TemporaryDirectory(prefix="crossink-integ-") as tmp:
        fs = SimFs(Path(tmp), manifest["kosync"])  # no open book: boots to home
        script = ";".join([
            "6000:DOWN", "6500:DOWN", "7000:CONFIRM",     # home menu -> BookOrbit catalog
            "11000:DOWN", "11350:DOWN", "11700:DOWN",     # root -> Collections (4th row,
            "12100:CONFIRM",                              #   after Libraries)
            "14500:CONFIRM",                              # the seeded collection
            "17500:QUIT",
        ])
        run_simulator(fs, input_script=script, choice="apply", timeout_s=60, verbose=verbose)

        caches = [json.loads(p.read_text())
                  for p in sorted((fs.crosspoint / "bookorbit_lists").glob("*.json"))]
        roots = [c for c in caches if "sections" in c]
        assert roots and any(s["id"] == "collections" for s in roots[0]["sections"]), \
            f"Collections missing from the cached root: {roots}"
        facets = [c for c in caches if "hasNext" in c]
        assert any(i["title"] == collection["name"] for c in facets for i in c["items"]), \
            f"collection list never loaded: {facets}"
        books = [c for c in caches if "total" in c and "sections" not in c]
        assert books, "collection books were never listed"
        listed = {b["title"] for b in books[0]["items"]}
        expected = {Path(b["file"]).stem for b in collection["books"]}
        assert listed == expected, f"collection books mismatch: {listed} != {expected}"


def scenario_catalog_empty_listing_back(verbose: bool) -> None:
    """An empty listing (a collection with no books) shows the no-entries error,
    and Back must climb out of it — the regression had the error screen's Back
    reload the very listing it was showing, forever. Proven by navigating onward
    after the error: the non-empty collection's books still load."""
    manifest, _ = load_seed()
    collection = manifest["collection"]

    with tempfile.TemporaryDirectory(prefix="crossink-integ-") as tmp:
        fs = SimFs(Path(tmp), manifest["kosync"])  # no open book: boots to home
        script = ";".join([
            "6000:DOWN", "6500:DOWN", "7000:CONFIRM",     # home menu -> BookOrbit catalog
            "11000:DOWN", "11350:DOWN", "11700:DOWN",     # root -> Collections (4th row,
            "12100:CONFIRM",                              #   after Libraries)
            "14500:DOWN", "14900:CONFIRM",                # Zero Shelf (empty) -> error screen
            "17500:BACK",                                 # must climb back to the collection list
            "19500:CONFIRM",                              # Integration Shelf -> its books
            "22500:QUIT",
        ])
        run_simulator(fs, input_script=script, choice="apply", timeout_s=60, verbose=verbose)

        caches = [json.loads(p.read_text())
                  for p in sorted((fs.crosspoint / "bookorbit_lists").glob("*.json"))]
        books = [c for c in caches if "total" in c and "sections" not in c]
        assert any(c["total"] == 0 for c in books), "the empty collection was never opened"
        expected = {Path(b["file"]).stem for b in collection["books"]}
        assert any({i["title"] for i in c["items"]} == expected for c in books), \
            "navigation after the empty-listing error never reached the collection's books"


def scenario_catalog_libraries_browse(verbose: bool) -> None:
    """Libraries is a browsable root section, like BookOrbit's own KOReader
    plugin: its listing carries each library's book count, and opening one lists
    that library's books (the query carries the library id in its cache key).
    Also proves the root's section counts: the dashboard fetch is cached with
    the listings. The seed has a single library holding every book."""
    manifest, books = load_seed()

    with tempfile.TemporaryDirectory(prefix="crossink-integ-") as tmp:
        fs = SimFs(Path(tmp), manifest["kosync"])  # no open book: boots to home
        script = ";".join([
            "6000:DOWN", "6500:DOWN", "7000:CONFIRM",  # home menu -> BookOrbit catalog
            "11000:DOWN", "11350:DOWN",                # root -> Libraries (3rd row)
            "11700:CONFIRM",
            "14000:CONFIRM",                           # the seeded library -> its books
            "17000:QUIT",
        ])
        run_simulator(fs, input_script=script, choice="apply", timeout_s=60, verbose=verbose)

        caches = [json.loads(p.read_text())
                  for p in sorted((fs.crosspoint / "bookorbit_lists").glob("*.json"))]
        counts = [c for c in caches if "totalBooks" in c]
        assert counts and counts[0]["totalBooks"] == len(books), \
            f"section counts never cached or wrong: {counts}"
        assert counts[0]["collections"] == 2, f"collections count wrong: {counts[0]}"
        facets = [c for c in caches if "hasNext" in c]
        library_rows = [i for c in facets for i in c["items"] if i["title"] == "Integration"]
        assert library_rows, f"the libraries listing never loaded: {facets}"
        # The per-library count comes from the server and covers every format, so
        # it includes the seeded audiobook-only book the listing itself hides.
        assert library_rows[0]["count"] == len(books) + 1, \
            f"library book count wrong: {library_rows[0]['count']} != {len(books) + 1}"
        scoped = [c for c in caches if c.get("key", "").split("|")[-1] != "" and "total" in c]
        assert scoped, "no book listing was fetched with a libraryId"
        assert scoped[0]["total"] == len(books), \
            f"library books total wrong: {scoped[0]['total']} != {len(books)}"


def scenario_catalog_smart_scopes_browse(verbose: bool) -> None:
    """SmartScopes is a browsable root section, like BookOrbit's own KOReader
    plugin: its listing names each scope, and opening one lists the books the
    scope resolves to (the query carries the scope id in its cache key). The
    seeded scope selects the Integration Shelf's members, so its books are the
    collection's -- reached through a saved server-side filter rather than
    through collection membership."""
    manifest, _ = load_seed()
    scope = manifest["smart_scope"]

    with tempfile.TemporaryDirectory(prefix="crossink-integ-") as tmp:
        fs = SimFs(Path(tmp), manifest["kosync"])  # no open book: boots to home
        script = ";".join([
            "6000:DOWN", "6500:DOWN", "7000:CONFIRM",     # home menu -> BookOrbit catalog
            "11000:DOWN", "11350:DOWN", "11700:DOWN",     # root -> SmartScopes (5th row,
            "12050:DOWN", "12400:CONFIRM",                #   after Collections)
            "14500:CONFIRM",                              # the seeded scope -> its books
            "17500:QUIT",
        ])
        run_simulator(fs, input_script=script, choice="apply", timeout_s=60, verbose=verbose)

        caches = [json.loads(p.read_text())
                  for p in sorted((fs.crosspoint / "bookorbit_lists").glob("*.json"))]
        roots = [c for c in caches if "sections" in c]
        assert roots and any(s["id"] == "smart-scopes" for s in roots[0]["sections"]), \
            f"SmartScopes missing from the cached root: {roots}"
        counts = [c for c in caches if "totalBooks" in c]
        assert counts and counts[0]["smartScopes"] == 1, \
            f"the root's SmartScopes count is wrong: {counts}"
        facets = [c for c in caches if "hasNext" in c]
        assert any(i["title"] == scope["name"] for c in facets for i in c["items"]), \
            f"the SmartScopes listing never loaded: {facets}"
        books = [c for c in caches if "total" in c and "sections" not in c]
        assert books, "the scope's books were never listed"
        listed = {b["title"] for b in books[0]["items"]}
        expected = {Path(b["file"]).stem for b in scope["books"]}
        assert listed == expected, f"SmartScope books mismatch: {listed} != {expected}"


def scenario_catalog_hides_non_epub(verbose: bool) -> None:
    """The catalog lists only books it can download: the audiobook-only record,
    which shares an EPUB's title and author, is absent, while the book holding
    both an EPUB and an audio file stays. The seed uploads the audiobook after
    the library and marks it as being read, so an unfiltered listing would show
    it first in Recently added and among Continue reading. The root's two book
    counts must match those EPUB listings, not the dashboard's all-format
    totals, and Continue reading lists only the books being read."""
    manifest, _ = load_seed()
    non_epub = manifest["non_epub"]
    server = KosyncDevice(BASE_URL, manifest["kosync"]["username"],
                          manifest["kosync"]["password"], SIM_DEVICE_ID)
    epub_total = server.catalog_page(size=1, format="epub")["total"]
    reading_total = server.catalog_page(size=1, format="epub", readStatus="reading")["total"]
    # Guards on the fixture itself: without these gaps the test proves nothing.
    assert server.catalog_page(size=1)["total"] > epub_total, "the seed holds no non-EPUB book"
    assert server.catalog_page(size=1, readStatus="reading")["total"] > reading_total, \
        "the seeded audiobook is not marked as being read"
    assert reading_total <= 20, "Continue reading no longer fits on one page; check whole pages"

    with tempfile.TemporaryDirectory(prefix="crossink-integ-") as tmp:
        fs = SimFs(Path(tmp), manifest["kosync"])  # no open book: boots to home
        script = ";".join([
            "6000:DOWN", "6500:DOWN", "7000:CONFIRM",  # home menu -> BookOrbit catalog
            "11000:CONFIRM",                           # root -> Continue reading (1st row)
            "14000:BACK",                              # back to the root
            "16000:DOWN", "16350:CONFIRM",             # Recently added (2nd row)
            "19500:QUIT",
        ])
        run_simulator(fs, input_script=script, choice="apply", timeout_s=60, verbose=verbose)

        caches = [json.loads(p.read_text())
                  for p in sorted((fs.crosspoint / "bookorbit_lists").glob("*.json"))]
        counts = [c for c in caches if "totalBooks" in c]
        assert counts, "the root's section counts were never cached"
        assert counts[0]["totalBooks"] == epub_total, \
            f"All books count {counts[0]['totalBooks']} != {epub_total} EPUBs"
        assert counts[0]["inProgress"] == reading_total, \
            f"Continue reading count {counts[0]['inProgress']} != {reading_total} EPUBs being read"

        # Book cache keys are books|page|sort|readStatus|...
        listings = {tuple(c["key"].split("|")[2:4]): c for c in caches
                    if c.get("key", "").startswith("books|")}
        reading = listings.get(("recently_read", "reading"))
        recent = listings.get(("recently_added", ""))
        assert reading, f"Continue reading never listed with readStatus=reading: {sorted(listings)}"
        assert recent, f"Recently added was never listed: {sorted(listings)}"
        assert reading["total"] == reading_total, \
            f"Continue reading total {reading['total']} != {reading_total}"
        assert recent["total"] == epub_total, f"Recently added total {recent['total']} != {epub_total}"

        reading_ids = {i["id"] for i in reading["items"]}
        recent_ids = {i["id"] for i in recent["items"]}
        assert non_epub["audiobook_id"] not in reading_ids | recent_ids, \
            f"the audiobook-only {non_epub['audiobook_title']!r} is listed"
        assert non_epub["mixed_id"] in reading_ids, \
            f"the EPUB+audio {non_epub['mixed_title']!r} is missing from Continue reading"


def scenario_catalog_download_naming(verbose: bool) -> None:
    """A catalog download lands where the account's KOReader file naming template
    says, not under the "Title - Author.epub" this firmware used to hardcode. The
    server resolves the template per file and ships it as the file's devicePath, so
    the expected path is asked of the server rather than recomputed here: the test
    proves the firmware honours the answer, folders included."""
    manifest, _ = load_seed()
    sim = KosyncDevice(BASE_URL, manifest["kosync"]["username"],
                       manifest["kosync"]["password"], SIM_DEVICE_ID)

    with tempfile.TemporaryDirectory(prefix="crossink-integ-") as tmp:
        # A configured download folder: the template's folders hang off it, and
        # neither it nor they exist on this card yet.
        fs = SimFs(Path(tmp), manifest["kosync"], download_folder="/Downloads")
        script = ";".join([
            "6000:DOWN", "6500:DOWN", "7000:CONFIRM",     # home menu -> BookOrbit catalog
            "11000:DOWN", "11350:DOWN", "11700:DOWN",     # root -> All books (8th row,
            "12050:DOWN", "12400:DOWN", "12750:DOWN",     #   after the facet sections,
            "13100:DOWN",                                 #   SmartScopes among them)
            "13450:CONFIRM",
            "16000:CONFIRM",                              # first book -> download
            "24000:QUIT",
        ])
        run_simulator(fs, input_script=script, choice="apply", timeout_s=90, verbose=verbose)

        # The book that was downloaded is the first row of the listing the browser
        # cached, so the expectation follows the device's own view of the catalog
        # instead of assuming how the server ordered it.
        caches = [json.loads(p.read_text())
                  for p in sorted((fs.crosspoint / "bookorbit_lists").glob("*.json"))]
        listings = [c for c in caches if "total" in c and "sections" not in c]
        assert listings and listings[0]["items"], f"no book listing was cached: {caches}"
        first = listings[0]["items"][0]
        epub = next(f for f in sim.catalog_book_detail(first["id"])["files"]
                    if f["format"].lower() == "epub")
        device_path = epub["devicePath"]
        assert "/" in device_path, \
            f"the seeded naming pattern should nest the book, got {device_path!r}"

        expected = fs.fs / "Downloads" / device_path
        downloaded = sorted(p.relative_to(fs.fs).as_posix() for p in fs.fs.rglob("*.epub"))
        assert expected.exists(), \
            f"{first['title']!r} did not download to {device_path!r}; EPUBs on the card: {downloaded}"
        assert expected.stat().st_size > 0, "the downloaded file is empty"
        # The legacy flat name must not appear beside it.
        assert downloaded == [f"Downloads/{device_path}"], \
            f"unexpected extra downloads: {downloaded}"

        # Second run on the same card: the offline "On device" category must still
        # find the book now that the template filed it two folders deep. The catalog
        # root counts it on entry, which is where the scan reports what it found.
        script = ";".join([
            "6000:DOWN", "6500:DOWN", "7000:CONFIRM",  # home menu -> BookOrbit catalog
            "13000:QUIT",                              # the root counts local books as it builds
        ])
        output = run_simulator(fs, input_script=script, choice="apply", timeout_s=60, verbose=verbose)
        assert "Local scan: kind=on-device count=1" in output, \
            "'On device' did not find the downloaded book inside the template's folders"


def scenario_catalog_book_actions(verbose: bool) -> None:
    """A catalog book's action menu, opened by holding Confirm, and what Confirm does
    once the book is on the device. Four runs on one card, each on the first book of
    All books: the menu's single Download entry downloads it; the on-device menu's
    Re-download replaces it in place, keeping its reading progress; a plain Confirm
    then opens it instead of downloading it again; and the menu's Delete removes it
    along with its cache."""
    manifest, _ = load_seed()
    sim = KosyncDevice(BASE_URL, manifest["kosync"]["username"],
                       manifest["kosync"]["password"], SIM_DEVICE_ID)
    to_all_books = [
        "6000:DOWN", "6500:DOWN", "7000:CONFIRM",     # home menu -> BookOrbit catalog
        "11000:DOWN", "11350:DOWN", "11700:DOWN",     # root -> All books (8th row)
        "12050:DOWN", "12400:DOWN", "12750:DOWN",
        "13100:DOWN",
        "13450:CONFIRM",
    ]
    hold_first_book = "16000:CONFIRM:1500"  # past the 1 s threshold: opens the menu
    # The menu logs the FileBrowserAction it returns: without that, a hold that
    # simply downloaded on release (the old behaviour) would pass steps 1 and 2.
    action_download, action_redownload, action_delete = 20, 21, 0

    with tempfile.TemporaryDirectory(prefix="crossink-integ-") as tmp:
        fs = SimFs(Path(tmp), manifest["kosync"], download_folder="/Downloads")

        # 1. Not on the device: the menu holds Download only, selected by default.
        script = ";".join(to_all_books + [hold_first_book, "18500:CONFIRM", "26500:QUIT"])
        output = run_simulator(fs, input_script=script, choice="apply", timeout_s=90, verbose=verbose)
        caches = [json.loads(p.read_text())
                  for p in sorted((fs.crosspoint / "bookorbit_lists").glob("*.json"))]
        listings = [c for c in caches if "total" in c and "sections" not in c]
        assert listings and listings[0]["items"], f"no book listing was cached: {caches}"
        first = listings[0]["items"][0]
        assert f"Book action {action_download} on book {first['id']}" in output, \
            "holding Confirm did not offer Download from the book's menu"
        epub = next(f for f in sim.catalog_book_detail(first["id"])["files"]
                    if f["format"].lower() == "epub")
        sim_path = f"/Downloads/{epub['devicePath']}"
        book = fs.fs / sim_path.lstrip("/")
        assert book.exists(), \
            f"the menu's Download did not fetch {first['title']!r} to {sim_path!r}"
        original = book.read_bytes()

        # 2. On the device: Re-download (second entry) replaces the file where it is.
        # Damage it without changing its size (the download index checks the size, so
        # the book stays recognised as on the device), and give it reading progress.
        damaged = bytearray(original)
        damaged[-64:] = bytes(64)
        book.write_bytes(bytes(damaged))
        progress = fs.epub_cache_dir(sim_path) / "progress.bin"
        write_progress_bin(progress, 3, 2, 10)
        script = ";".join(to_all_books + [hold_first_book, "18500:DOWN", "18900:CONFIRM", "27000:QUIT"])
        output = run_simulator(fs, input_script=script, choice="apply", timeout_s=90, verbose=verbose)
        assert f"Book action {action_redownload} on book {first['id']}" in output, \
            "the on-device menu's second entry is not Re-download"
        assert book.read_bytes() == original, "Re-download did not replace the damaged file"
        epubs = sorted(p.relative_to(fs.fs).as_posix() for p in fs.fs.rglob("*.epub*"))
        assert epubs == [sim_path.lstrip("/")], f"Re-download left other files behind: {epubs}"
        kept = read_progress_bin(progress)
        assert kept and (kept["spine"], kept["page"]) == (3, 2), \
            f"Re-download lost the book's reading progress: {kept}"

        # 3. A plain Confirm opens the on-device book. The catalog persists the path and
        # silent-restarts into the reader; synthetic input does not survive that
        # restart, so the run ends on the timeout.
        script = ";".join(to_all_books + ["16000:CONFIRM"])
        output = run_simulator(fs, input_script=script, choice="apply", timeout_s=30, verbose=verbose)
        assert "Downloading file" not in output, "Confirm downloaded an on-device book again"
        state = json.loads((fs.crosspoint / "state.json").read_text())
        assert state.get("openEpubPath") == sim_path, \
            f"Confirm did not open {sim_path!r}: state={state}"
        assert book.read_bytes() == original, "opening the book changed the file"

        # 4. Delete (third entry), then confirm (Cancel is preselected). Boot to home
        # again first: the reader left the book open and on the recent list, which
        # would reshape the home screen the navigation counts on.
        for name in ("state.json", "state.bin", "recent.json", "recent.bin"):
            (fs.crosspoint / name).unlink(missing_ok=True)
        script = ";".join(to_all_books + [
            hold_first_book, "18500:DOWN", "18850:DOWN", "19200:CONFIRM",
            "21000:DOWN", "21400:CONFIRM",
            "23500:QUIT",
        ])
        output = run_simulator(fs, input_script=script, choice="apply", timeout_s=60, verbose=verbose)
        assert f"Book action {action_delete} on book {first['id']}" in output, \
            "the on-device menu's third entry is not Delete"
        assert not book.exists(), "Delete left the book on the card"
        assert not fs.epub_cache_dir(sim_path).exists(), "Delete left the book's cache behind"


def scenario_bookmark_push(verbose: bool) -> None:
    """A local bookmark (store entry + minted position record) reaches the
    server and is offered to a device that has never seen it."""
    manifest, books = load_seed()
    book = books[FRESH_BOOK["bookmark_push"]]
    # Fixed identity across runs (see BOOKMARK_PUSH_EPOCH): re-runs upload the same
    # key and the server treats it as the same bookmark, as with a real device.
    epoch = BOOKMARK_PUSH_EPOCH
    pos = "/body/DocFragment[3]/body/p[1]/text().0"

    with tempfile.TemporaryDirectory(prefix="crossink-integ-") as tmp:
        fs, sim_path, _ = make_fs(tmp, manifest, book)
        # Unlike clippings, bookmark timestamps are real WallClock epochs and
        # double as the sync identity.
        write_bookmark_store(fs.bookmarks_store(book["hash"]), book.get("title", ""),
                             book.get("author", ""), sim_path,
                             [{"spine": 2, "progress": 0.25, "timestamp": epoch,
                               "chapter": "Chapter", "paragraph": 1, "snippet": "Local bookmark"}])
        write_bob_store(fs.state_dir(book["hash"]) / BOOKMARK_STORE, 0, [{
            "timestamp": epoch, "identityEpoch": epoch, "spine": 2, "pos": pos,
        }])

        output = run_simulator(fs, input_script="6000:POWER:120", choice="upload",
                               timeout_s=90, verbose=verbose)
        assert "sync scenario finished" in output, "sync never reached its end marker"
        watermark, _ = read_bob_store(fs.state_dir(book["hash"]) / BOOKMARK_STORE)
        assert watermark >= epoch, f"upload watermark did not advance: {watermark} < {epoch}"

    adds = bookmark_adds(verify_device(manifest, "bmpush"), book["hash"])
    assert any(a["pos"] == pos for a in adds), \
        f"pushed bookmark not on server; offered={[a['pos'] for a in adds]}"


SCENARIOS = {
    "catalog_collections_browse": scenario_catalog_collections_browse,
    "catalog_empty_listing_back": scenario_catalog_empty_listing_back,
    "catalog_libraries_browse": scenario_catalog_libraries_browse,
    "catalog_smart_scopes_browse": scenario_catalog_smart_scopes_browse,
    "catalog_download_naming": scenario_catalog_download_naming,
    "catalog_book_actions": scenario_catalog_book_actions,
    "catalog_hides_non_epub": scenario_catalog_hides_non_epub,
    "sync_progress_pull": scenario_sync_progress_pull,
    "sync_progress_push": scenario_sync_progress_push,
    "highlight_pull": scenario_highlight_pull,
    "highlight_push": scenario_highlight_push,
    "highlight_delete_propagates": scenario_highlight_delete_propagates,
    "highlight_delete_guard": scenario_highlight_delete_guard,
    "bookmark_pull": scenario_bookmark_pull,
    "bookmark_push": scenario_bookmark_push,
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
