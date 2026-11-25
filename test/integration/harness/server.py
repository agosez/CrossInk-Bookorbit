"""The server side: what seed/seed.py left on it, and the clients that read and change it.

The synthetic peer that seeded the server is the scenarios' "other device": the
simulator's MAC is fixed, so two simulators would share one device id.
"""

from __future__ import annotations

import contextlib
import http.server
import json
import threading
import time
import urllib.error
import urllib.request
from pathlib import Path

from kosync import BASE_URL, AdminClient, KosyncDevice, request_json
from sim import INTEGRATION, SIM_DEVICE_ID


def load_seed() -> tuple[dict, list[dict]]:
    manifest = json.loads((INTEGRATION / "seed-manifest.json").read_text())
    library = json.loads((INTEGRATION / "library.json").read_text())
    return manifest, library


# Library indices for scenarios that need a book with NO seeded server state,
# outside every seeded range in seed/seed.py (progress 0-9, highlights 10-19,
# bookmarks 20-24, collection 40-44). One book per scenario so runs never
# contaminate each other.
FRESH_BOOK = {
    "highlight_push": 30,
    "bookmark_push": 31,
    "highlight_delete": 32,
    "highlight_delete_guard": 33,
    "bookmark_pull": 34,
    "highlight_mint_outside_paragraph": 35,
    "highlight_delete_all": 36,
    "highlight_note": 37,
    "bookmark_delete_all": 38,
    "highlight_reoffer": 39,
    "highlight_push_backlog": 45,
    "highlight_pull_backlog": 46,
    "highlight_delete_propagates_large": 47,
    "outdated_copy": 49,
    "book_update": 48,
    "book_update_offer": 51,
    "bookmark_pull_same_chapter": 50,
    "bookmark_recover_lost": 52,
}


# --- devices -------------------------------------------------------------------------


def device(manifest: dict, device_id: str, auth: bool = False) -> KosyncDevice:
    """A kosync client of the seeded account, as `device_id`."""
    dev = KosyncDevice(BASE_URL, manifest["kosync"]["username"], manifest["kosync"]["password"], device_id)
    if auth:
        dev.auth()
    return dev


def sim_device(manifest: dict, auth: bool = False) -> KosyncDevice:
    """The simulator's own device id: the server's view of it, or acting in its name."""
    return device(manifest, SIM_DEVICE_ID, auth)


def peer_device(manifest: dict) -> KosyncDevice:
    """The synthetic 'other reader' that seeded the server-side state."""
    return device(manifest, manifest["peer_device_id"], auth=True)


def verify_device(manifest: dict, tag: str) -> KosyncDevice:
    """A never-seen device id: the server offers it everything it has no sync
    state for, which makes it an honest view of what exists server-side."""
    return device(manifest, f"crossink-verify-{tag}-{int(time.time())}", auth=True)


def server_accepts_sidecar_state(dev: KosyncDevice) -> bool:
    """BookOrbit 3.5.0 and later: the exchanges take `sidecarState`, and a lost copy can be
    asked back. Older servers reject the field with HTTP 400."""
    info = request_json("GET", dev.api("/plugin/version"), dev._headers(), ok=(200,))
    return "sidecarState" in info.get("capabilities", [])


# --- what the server holds -----------------------------------------------------------


def annotation_adds(dev: KosyncDevice, book_hash: str) -> list[dict]:
    resp = dev.exchange_annotations(book_hash, keys=[], keys_complete=False, changes=[])
    assert not resp.get("unmatched"), f"server does not know book {book_hash}: {resp}"
    return resp["results"][0]["toApply"]["add"]


def bookmark_adds(dev: KosyncDevice, book_hash: str) -> list[dict]:
    resp = dev.exchange_bookmarks(book_hash, keys=[], keys_complete=False, changes=[])
    assert not resp.get("unmatched"), f"server does not know book {book_hash}: {resp}"
    return resp["results"][0]["toApply"]["add"]


def offered_annotation_texts(dev: KosyncDevice, book_hash: str) -> set[str]:
    """Every highlight the server offers a never-seen device for a book. The offer comes
    a page at a time, and acknowledging a page is what lets the next one through."""
    texts: set[str] = set()
    for _ in range(50):
        resp = dev.exchange_annotations(book_hash, keys=[], keys_complete=False, changes=[])
        assert not resp.get("unmatched"), f"server does not know book {book_hash}: {resp}"
        result = resp["results"][0]
        adds = result["toApply"]["add"]
        texts.update(a["text"] for a in adds)
        if not result.get("more") or not adds:
            return texts
        dev.exchange_annotations_ack({
            "deviceId": dev.device_id,
            "deviceModel": dev.device_model,
            "pluginVersion": dev.plugin_version,
            "books": [{"hash": book_hash, "deleted": [], "applied": [
                {"serverId": a["serverId"], "version": a["version"], "status": "applied"} for a in adds]}],
        })
    raise AssertionError(f"the server kept offering pages for book {book_hash}")


def want_to_read_epubs(dev: KosyncDevice) -> list[int]:
    """The EPUBs on the account's Want to Read shelf, in the server's order."""
    shelf = request_json("GET", dev.api("/plugin/catalog/dashboard/sections/want-to-read"),
                         dev._headers(), ok=(200,))
    return [b["id"] for b in shelf["section"]["books"] if "epub" in [f.lower() for f in b.get("formats", [])]]


# --- changes made through BookOrbit's own web API ------------------------------------


def admin_book(manifest: dict, book: dict) -> tuple[AdminClient, int]:
    """BookOrbit's own web API as the seeded user, and the id it lists the book under."""
    admin = AdminClient(BASE_URL)
    admin.login(manifest["user"]["username"], manifest["user"]["password"])
    # A book is titled by its file name until the server's metadata scan reaches it.
    ids = admin.find_book_ids([book["title"], Path(book["file"]).stem])
    assert ids, f"the server does not list {book['file']}"
    return admin, next(iter(ids.values()))


def web_session(manifest: dict, book: dict) -> tuple[AdminClient, int]:
    """BookOrbit's own web API as the seeded user, and the id it knows the book's EPUB
    file by."""
    admin, book_id = admin_book(manifest, book)
    return admin, admin.epub_file_id(book_id)


def open_in_web_reader(manifest: dict, book: dict, xpointer: str, percentage: float) -> None:
    """Open the book in BookOrbit's web reader and close it without reading. The
    reader saves the position it lands on two seconds after opening, which dates the
    server's position as of now although nobody read anything."""
    admin, file_id = web_session(manifest, book)
    admin.save_web_progress(file_id, xpointer, percentage)


def rewrite_book_file(manifest: dict, book: dict) -> None:
    """BookOrbit writes its metadata into the book's file, as editing a book's details does
    with write-back on: the file's hash changes and the old one stays only as history
    (`file_write`). A device holding the library's original copy is then answered
    `positionIncompatible`. Rewriting again on a re-run keeps that original hash in history."""
    admin, book_id = admin_book(manifest, book)
    result = request_json("POST", admin.api(f"/books/{book_id}/write-and-rename"),
                          admin._headers(), {}, ok=(200, 201))
    assert result.get("write", {}).get("status") == "success", f"metadata write-back failed: {result}"


def reset_sim_bookmarks(manifest: dict, book_hash: str) -> None:
    """Detach the simulator's fixed device id from every bookmark on this book, so the
    next peer bookmark is offered to it again (the server offers a bookmark only to
    devices holding no link for it). Impersonates the simulator: an empty-complete key
    set tombstones what it held and drops those links; acking the returned deletion
    offers drops any stale links on already-tombstoned rows."""
    sim = sim_device(manifest, auth=True)
    resp = sim.exchange_bookmarks(book_hash, keys=[], keys_complete=True, changes=[])
    deletes = resp["results"][0]["toApply"]["delete"]
    if deletes:
        sim.exchange_bookmarks_ack(book_hash, deleted=[
            {"serverId": d["serverId"], "status": "applied"} for d in deletes])


@contextlib.contextmanager
def server_with_failing_progress():
    """A local stand-in for the server, yielding its URL: it forwards every request to the
    real one except those for reading progress, which it answers with a 500. The server
    is up, its dashboard answers, and a book's sync fails."""
    upstream = BASE_URL.rstrip("/")
    dropped = {"host", "connection", "accept-encoding", "content-length", "transfer-encoding"}

    class Handler(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, *args) -> None:
            pass

        def forward(self) -> None:
            length = int(self.headers.get("Content-Length") or 0)
            body = self.rfile.read(length) if length else None
            if "/syncs/progress" in self.path:
                status, headers, data = 500, {"Content-Type": "application/json"}, b'{"message":"injected"}'
            else:
                request = urllib.request.Request(
                    upstream + self.path, data=body, method=self.command,
                    headers={k: v for k, v in self.headers.items() if k.lower() not in dropped})
                try:
                    with urllib.request.urlopen(request, timeout=30) as response:
                        status, headers, data = response.status, dict(response.headers), response.read()
                except urllib.error.HTTPError as error:
                    status, headers, data = error.code, dict(error.headers), error.read()
            self.send_response(status)
            for key, value in headers.items():
                if key.lower() not in dropped:
                    self.send_header(key, value)
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        do_GET = do_PUT = do_POST = do_DELETE = do_PATCH = forward

    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        yield f"http://127.0.0.1:{server.server_address[1]}"
    finally:
        server.shutdown()
        server.server_close()
