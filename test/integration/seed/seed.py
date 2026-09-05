#!/usr/bin/env python3
"""Seed the integration BookOrbit server.

Idempotent: safe to re-run against an already-seeded server. Phases:

1. Wait for the server to be healthy.
2. Bootstrap the test user (setup token) and its KOReader sync credentials.
3. Wait for the book dock to ingest the generated library.
4. As a synthetic second device, pre-load state on a fixed subset of books:
   reading progress on some, highlights and bookmarks on others, so scenarios
   can pull them down to the simulator or race against them.

The synthetic-device state is described in seed-manifest.json for the harness.
"""

from __future__ import annotations

import json
import shutil
import sys
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

    peer = KosyncDevice(BASE_URL, KOSYNC["username"], KOSYNC["password"], OTHER_DEVICE_ID)
    peer.auth()

    manifest = SeedManifest(INTEGRATION / "seed-manifest.json")
    manifest.data = {"user": USER, "kosync": KOSYNC, "peer_device_id": OTHER_DEVICE_ID,
                     "progress": [], "highlights": [], "bookmarks": [],
                     "collection": {"id": collection_id, "name": "Integration Shelf",
                                    "books": collection_books},
                     "empty_collection": {"id": empty_collection_id, "name": "Zero Shelf"}}

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
