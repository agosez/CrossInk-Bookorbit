#!/usr/bin/env python3
"""Seed the integration BookOrbit server.

Idempotent: safe to re-run against an already-seeded server. Phases:

1. Wait for the server to be healthy.
2. Bootstrap the test user (setup token) and its KOReader sync credentials.
3. Wait for the book dock to ingest the generated library.
4. Add the records the catalog must hide or keep: an audiobook-only book that
   shares an EPUB's title and author, and an EPUB that also carries an
   audiobook file. Both are marked as being read.
5. As a synthetic second device, pre-load state on a fixed subset of books:
   reading progress on some, highlights and bookmarks on others, so scenarios
   can pull them down to the simulator or race against them.

The synthetic-device state is described in seed-manifest.json for the harness.
"""

from __future__ import annotations

import json
import shutil
import struct
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from kosync import BASE_URL, SETUP_TOKEN, AdminClient, KosyncDevice, SeedManifest, kodatetime  # noqa: E402

INTEGRATION = Path(__file__).resolve().parents[1]

USER = {"username": "crossink", "name": "CrossInk Integration", "email": "crossink@example.test",
        "password": "CrossinkTest1"}
KOSYNC = {"username": "crossink-sync", "password": "CrossinkSync1"}
# The synthetic "other reader". Distinct from the simulator's fixed
# crossink-deadbeef0001 so two-device scenarios are honest.
OTHER_DEVICE_ID = "crossink-integration-peer"

# Book indices (into library.json order) that get pre-seeded state.
WITH_PROGRESS = range(0, 10)     # server progress at ~40%
WITH_HIGHLIGHTS = range(10, 20)  # two highlights each
WITH_BOOKMARKS = range(20, 25)   # one bookmark each
IN_COLLECTION = range(40, 45)    # members of the "Integration Shelf" collection
SEED_EPOCH = 1_756_000_000       # fixed timestamps keep reruns idempotent
# The account's KOReader file naming template, which catalog downloads must follow.
NAMING_PATTERN = "Catalog/{authors:first}/{authors:first} - {title}"
# The SmartScope the catalog's smart-scopes section is browsed through; it mirrors
# the collection above, so both sections list the same books by different means.
SMART_SCOPE_NAME = "Integration Scope"
# Non-EPUB records, outside every other seeded range: the audiobook-only book
# copies this EPUB's title and author, the way a library holds a novel and its
# audiobook as separate records; the mixed-format book gets an audio file too.
AUDIOBOOK_TWIN = 60
MIXED_FORMAT = 70


def silent_mp3(path: Path, title: str, artist: str, seconds: int = 1) -> None:
    """A valid, silent MP3 carrying an ID3v2.3 title and artist. Every frame is an
    MPEG-1 Layer III header (32 kbit/s, 44.1 kHz, mono) followed by zeroed side
    info and main data, which decoders play as silence; no encoder needed."""
    def text_frame(frame_id: str, text: str) -> bytes:
        data = b"\x00" + text.encode("latin-1")  # encoding byte: ISO-8859-1
        return frame_id.encode() + struct.pack(">I", len(data)) + b"\x00\x00" + data

    frames = text_frame("TIT2", title) + text_frame("TPE1", artist)
    size = len(frames)
    syncsafe = bytes([(size >> 21) & 0x7F, (size >> 14) & 0x7F, (size >> 7) & 0x7F, size & 0x7F])
    tag = b"ID3\x03\x00\x00" + syncsafe + frames
    frame = b"\xff\xfb\x10\xc0" + bytes(104 - 4)  # 144 * 32000 / 44100 = 104 bytes
    path.write_bytes(tag + frame * (seconds * 39))  # 1152 samples per frame


def seed_non_epub_books(admin: AdminClient, peer: KosyncDevice, library_id: int,
                        books: list[dict]) -> dict:
    """The audiobook-only twin and the mixed-format book, both marked as being
    read so they would also land in Continue reading. Idempotent: the twin is
    found again through the catalog's own format filter, and re-attaching the
    mixed book's audio file is refused as already attached."""
    twin, mixed = books[AUDIOBOOK_TWIN], books[MIXED_FORMAT]
    audio_only = [item for item in peer.catalog_page(format="mp3", size=50)["items"]
                  if item.get("formats") == ["mp3"]]
    with tempfile.TemporaryDirectory(prefix="crossink-seed-") as tmp:
        if audio_only:
            audiobook_id = int(audio_only[0]["id"])
        else:
            audio = Path(tmp) / f"{Path(twin['file']).stem}-audiobook.mp3"
            silent_mp3(audio, twin["title"], twin["author"])
            audiobook_id = admin.upload_book(library_id, audio)
            if audiobook_id is None:
                raise RuntimeError("the audiobook file exists server-side but no audio-only book lists it")

        names = [mixed["title"], Path(mixed["file"]).stem]
        ids = admin.find_book_ids(names)
        mixed_id = next((ids[n] for n in names if n in ids), None)
        if mixed_id is None:
            raise RuntimeError(f"Book '{mixed['title']}' not found on the server")
        audio = Path(tmp) / f"{Path(mixed['file']).stem}.mp3"
        silent_mp3(audio, mixed["title"], mixed["author"])
        admin.add_book_file(mixed_id, audio)

    for book_id in (audiobook_id, mixed_id):
        peer.set_read_status(book_id, "reading")
    return {"audiobook_id": audiobook_id, "audiobook_title": twin["title"],
            "mixed_id": mixed_id, "mixed_title": mixed["title"]}


def main() -> int:
    library_path = INTEGRATION / "library.json"
    if not library_path.exists():
        print("library.json missing; run seed/make_library.py first", file=sys.stderr)
        return 1
    books = json.loads(library_path.read_text())

    admin = AdminClient(BASE_URL)
    print("Waiting for server health...")
    admin.wait_healthy()

    # Login-first keeps reruns off the setup endpoint, which throttles hard.
    if admin.try_login(USER["username"], USER["password"]):
        print("Logged in (server already bootstrapped)")
    else:
        if admin.setup(SETUP_TOKEN, **USER):
            print(f"Bootstrapped user {USER['username']}")
        else:
            print("Server bootstrapped by someone else; trying login anyway")
        admin.login(USER["username"], USER["password"])
    admin.ensure_koreader_credentials(**KOSYNC)
    print("KOReader sync credentials in place")

    library_id, folder_id = admin.ensure_library("Integration", "/data/library")
    print(f"Library {library_id} (folder {folder_id}) in place")

    # The dock is an inbox: files land as pending rows, then an explicit
    # finalize files them into the library. Wait for the watcher to register
    # every file, finalize the lot, and wait for the books to materialize.
    if admin.book_count() < len(books):
        # The dock consumes its files on finalize, so it is fed from the local
        # reference copy each time; already-ingested duplicates are just re-filed
        # nowhere because the server already holds those hashes.
        dock = INTEGRATION / "library-dock"
        dock.mkdir(exist_ok=True)
        for entry in books:
            source = INTEGRATION / "library" / entry["file"]
            target = dock / entry["file"]
            if source.exists() and not target.exists():
                shutil.copy2(source, target)
        print("Waiting for the book dock to register the library files...")
        deadline = time.monotonic() + 600
        while True:
            pending = admin.dock_pending_count()
            if pending + admin.book_count() >= len(books):
                break
            if time.monotonic() > deadline:
                print(f"Dock registration stalled at {pending}", file=sys.stderr)
                return 1
            print(f"  {pending}/{len(books)} in dock")
            time.sleep(5)

        print("Finalizing dock files into the library...")
        deadline = time.monotonic() + 600
        while admin.book_count() < len(books):
            if admin.dock_pending_count() > 0:
                admin.dock_finalize_all(library_id, folder_id)
            if time.monotonic() > deadline:
                print(f"Finalize stalled at {admin.book_count()}/{len(books)} books", file=sys.stderr)
                return 1
            print(f"  {admin.book_count()}/{len(books)} books")
            time.sleep(5)
    print(f"Library ingested ({admin.book_count()} books)")

    # A collection for the catalog browser's Collections section. Books whose
    # metadata scan is still pending are titled by filename stem server-side, so
    # look each one up under both names.
    collection_books = [books[i] for i in IN_COLLECTION]
    names = {b["title"]: [b["title"], Path(b["file"]).stem] for b in collection_books}
    ids_by_title = admin.find_book_ids([n for pair in names.values() for n in pair])
    collection_ids = []
    for title, candidates in names.items():
        book_id = next((ids_by_title[n] for n in candidates if n in ids_by_title), None)
        if book_id is None:
            print(f"Book '{title}' not found on the server; cannot build the collection", file=sys.stderr)
            return 1
        collection_ids.append(book_id)
    collection_id = admin.ensure_collection("Integration Shelf", "book", collection_ids)
    # An EMPTY collection too: its book listing is the catalog's empty-listing
    # screen, which scenarios drive Back navigation through.
    empty_collection_id = admin.ensure_collection("Zero Shelf", "book", [])
    print(f"Collections {collection_id} (Integration Shelf) and {empty_collection_id} (Zero Shelf) in place")

    # A SmartScope for the catalog browser's SmartScopes section. It selects the
    # Integration Shelf's members by collection name -- the server's collection
    # rule matches on name, not id -- so the scope resolves to exactly the books
    # seeded above without depending on the metadata scan, which lags ingestion
    # and leaves most books' authors unindexed for a while.
    scope_id = admin.ensure_smart_scope(
        SMART_SCOPE_NAME, "sparkles",
        [{"type": "rule", "field": "collection", "operator": "includesAny", "value": ["Integration Shelf"]}])
    print(f"SmartScope {scope_id} ({SMART_SCOPE_NAME}) in place")

    # The account's KOReader file naming template. Catalog downloads must follow it,
    # so it deliberately asks for a folder the SD card does not have yet and for a
    # name unlike the "Title - Author.epub" this firmware used to hardcode.
    admin.set_file_naming_pattern(NAMING_PATTERN)
    print(f"KOReader file naming pattern set to {NAMING_PATTERN}")

    peer = KosyncDevice(BASE_URL, KOSYNC["username"], KOSYNC["password"], OTHER_DEVICE_ID)
    peer.auth()

    non_epub = seed_non_epub_books(admin, peer, library_id, books)
    print(f"Audiobook-only book {non_epub['audiobook_id']} and mixed-format book "
          f"{non_epub['mixed_id']} in place")

    manifest = SeedManifest(INTEGRATION / "seed-manifest.json")
    manifest.data = {"user": USER, "kosync": KOSYNC, "peer_device_id": OTHER_DEVICE_ID,
                     "progress": [], "highlights": [], "bookmarks": [],
                     "collection": {"id": collection_id, "name": "Integration Shelf",
                                    "books": collection_books},
                     "empty_collection": {"id": empty_collection_id, "name": "Zero Shelf"},
                     "smart_scope": {"id": scope_id, "name": SMART_SCOPE_NAME,
                                     "books": collection_books},
                     "naming_pattern": NAMING_PATTERN,
                     "non_epub": non_epub}

    for i in WITH_PROGRESS:
        book = books[i]
        # A mid-book xpointer position, the shape the firmware itself uploads.
        progress = "/body/DocFragment[6]/body/p[3]/text().0"
        peer.put_progress(book["hash"], progress, 0.40, timestamp=SEED_EPOCH)
        manifest.data["progress"].append({**book, "progress": progress, "percentage": 0.40})

    for n, i in enumerate(WITH_HIGHLIGHTS):
        book = books[i]
        changes = [
            {"datetime": kodatetime(SEED_EPOCH + n * 60 + k),
             "pos0": f"/body/DocFragment[4]/body/p[{2 + k}]/text().0",
             "pos1": f"/body/DocFragment[4]/body/p[{2 + k}]/text().20",
             "text": f"Seeded highlight {k + 1} for book {i + 1}",
             "chapter": "Chapter"}
            for k in range(2)
        ]
        peer.exchange_annotations(book["hash"], keys=[], keys_complete=False, changes=changes)
        manifest.data["highlights"].append({**book, "count": len(changes)})

    for n, i in enumerate(WITH_BOOKMARKS):
        book = books[i]
        peer.exchange_bookmarks(book["hash"], keys=[], keys_complete=False, changes=[{
            "datetime": kodatetime(SEED_EPOCH + 3600 + n),
            "pos": "/body/DocFragment[5]/body/p[1]/text().0",
        }])
        manifest.data["bookmarks"].append({**book, "count": 1})

    manifest.save()
    print(f"Seeded {len(manifest.data['progress'])} progressions, "
          f"{len(manifest.data['highlights'])} books with highlights, "
          f"{len(manifest.data['bookmarks'])} with bookmarks -> {manifest.path.name}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
