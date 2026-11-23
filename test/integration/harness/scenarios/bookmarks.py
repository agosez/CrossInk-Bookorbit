"""Bookmarks: the exchange with the server both ways, deletions, several bookmarks in one
chapter (issue #120), and the recovery of the ones builds before its fix lost."""

from __future__ import annotations

import hashlib
import time

from formats import bob_store_magic, read_bob_store, read_bookmark_store, write_bob_store, write_bookmark_store
from kosync import kodatetime
from server import FRESH_BOOK, bookmark_adds, load_seed, peer_device, reset_sim_bookmarks, sim_device, verify_device
from sim import sd_card, sync

# Bookmarks dedupe server-side on their converted location (paragraph precision), so a
# re-run "new" bookmark lands on the SAME server row. A row's identity key (datetime+pos)
# must therefore stay fixed across runs, exactly as a real device keeps a bookmark's
# minted datetime for life — re-minting one is what tombstones the row through the
# complete-key-set diff.
BOOKMARK_PUSH_EPOCH = 1_756_100_000


def bookmark_key(datetime: str, pos: str) -> str:
    """A bookmark's identity, md5(datetime | pos) (bookOrbitAnnotationKey)."""
    return hashlib.md5(f"{datetime}|{pos}".encode()).hexdigest()


def add_peer_bookmarks(manifest: dict, book_hash: str, positions: list[str], epoch: int) -> None:
    """Bookmarks the synthetic peer makes at `positions`, a second apart from `epoch`."""
    peer_device(manifest).exchange_bookmarks(book_hash, keys=[], keys_complete=False,
                                             changes=[{"datetime": kodatetime(epoch + i), "pos": pos}
                                                      for i, pos in enumerate(positions)])


def scenario_bookmark_pull() -> None:
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
    add_peer_bookmarks(manifest, book["hash"], [pos], now)

    with sd_card(manifest) as fs:
        fs.open_book(book)
        sync(fs, "apply")

        _, records = read_bob_store(fs.bookmark_sync_store(book["hash"]))
        assert any(r["pos"] == pos for r in records), \
            f"no position record for the received bookmark: {records}"
        bookmarks = read_bookmark_store(fs.bookmarks_store(book["hash"]))
        assert any(b["spine"] == 1 for b in bookmarks), \
            f"bookmark did not land in chapter 2: {bookmarks}"


def scenario_bookmark_push() -> None:
    """A local bookmark (store entry + minted position record) reaches the
    server and is offered to a device that has never seen it."""
    manifest, books = load_seed()
    book = books[FRESH_BOOK["bookmark_push"]]
    # Fixed identity across runs (see BOOKMARK_PUSH_EPOCH): re-runs upload the same
    # key and the server treats it as the same bookmark, as with a real device.
    epoch = BOOKMARK_PUSH_EPOCH
    pos = "/body/DocFragment[3]/body/p[1]/text().0"

    with sd_card(manifest) as fs:
        sim_path = fs.open_book(book)
        # Unlike clippings, bookmark timestamps are real WallClock epochs and
        # double as the sync identity.
        write_bookmark_store(fs.bookmarks_store(book["hash"]), book.get("title", ""),
                             book.get("author", ""), sim_path,
                             [{"spine": 2, "progress": 0.25, "timestamp": epoch,
                               "chapter": "Chapter", "paragraph": 1, "snippet": "Local bookmark"}])
        write_bob_store(fs.bookmark_sync_store(book["hash"]), 0, [{
            "timestamp": epoch, "identityEpoch": epoch, "spine": 2, "pos": pos,
        }])

        sync(fs, "upload")
        watermark, _ = read_bob_store(fs.bookmark_sync_store(book["hash"]))
        assert watermark >= epoch, f"upload watermark did not advance: {watermark} < {epoch}"

    adds = bookmark_adds(verify_device(manifest, "bmpush"), book["hash"])
    assert any(a["pos"] == pos for a in adds), \
        f"pushed bookmark not on server; offered={[a['pos'] for a in adds]}"


def scenario_bookmark_delete_all_propagates() -> None:
    """Deleting every synced bookmark of a book deletes them on the server: BookOrbit
    3.5.0 applies the same lost-copy reading to bookmarks as to highlights (see
    scenario_highlight_delete_all_propagates). A fresh book, so no earlier run left the
    simulator linked to anything here."""
    manifest, books = load_seed()
    book = books[FRESH_BOOK["bookmark_delete_all"]]
    now = int(time.time())
    # Different chapters: the server dedupes bookmarks on their converted location.
    positions = ["/body/DocFragment[2]/body/p[1]/text().0", "/body/DocFragment[3]/body/p[1]/text().0"]
    add_peer_bookmarks(manifest, book["hash"], positions, now)

    with sd_card(manifest) as fs:
        sim_path = fs.open_book(book)
        sync(fs, "apply", what="first sync")
        _, records = read_bob_store(fs.bookmark_sync_store(book["hash"]))
        assert {r["pos"] for r in records} >= set(positions), f"setup: bookmarks never arrived: {records}"

        write_bookmark_store(fs.bookmarks_store(book["hash"]), "", "", sim_path, [])

        sync(fs, "apply", what="second sync")
        bookmarks = read_bookmark_store(fs.bookmarks_store(book["hash"]))
        assert not bookmarks, f"the deleted bookmarks came back: {bookmarks}"

    adds = bookmark_adds(verify_device(manifest, "bmdelall"), book["hash"])
    assert not any(a["pos"] in positions for a in adds), \
        f"deleted bookmarks still on server: {[a['pos'] for a in adds]}"


def scenario_bookmark_pull_same_chapter() -> None:
    """Several bookmarks another reader made in ONE chapter all arrive, each at its own
    place, and survive the next sync on the server too (issue #120). They used to land at
    the chapter's start, where each one replaced the one before it, and the next sync's
    complete key set then deleted the replaced ones on the server."""
    manifest, books = load_seed()
    book = books[FRESH_BOOK["bookmark_pull_same_chapter"]]
    now = int(time.time())
    # Different paragraphs: the server dedupes bookmarks on their converted location.
    positions = [f"/body/DocFragment[2]/body/p[{p}]/text().0" for p in (2, 6, 10)]
    reset_sim_bookmarks(manifest, book["hash"])
    add_peer_bookmarks(manifest, book["hash"], positions, now)

    with sd_card(manifest) as fs:
        fs.open_book(book)
        sync(fs, "apply", what="first sync")

        _, records = read_bob_store(fs.bookmark_sync_store(book["hash"]))
        assert sorted(r["pos"] for r in records) == sorted(positions), \
            f"a received bookmark has no position record: {records}"
        assert len({r["timestamp"] for r in records}) == len(records), \
            f"received bookmarks share a timestamp, so their records collide: {records}"
        bookmarks = sorted(read_bookmark_store(fs.bookmarks_store(book["hash"])), key=lambda b: b["progress"])
        assert len(bookmarks) == len(positions) and all(b["spine"] == 1 for b in bookmarks), \
            f"the bookmarks of chapter 2 did not all arrive: {bookmarks}"
        assert len({b["progress"] for b in bookmarks}) == len(positions), \
            f"the received bookmarks were not placed apart: {bookmarks}"
        # p[2], p[6], p[10] are the chapter's 2nd, 6th and 10th paragraphs (all under <body>).
        assert [b["paragraph"] for b in bookmarks] == [2, 6, 10], \
            f"the received bookmarks lost their paragraphs: {bookmarks}"

        sync(fs, "apply", what="second sync")
        assert len(read_bookmark_store(fs.bookmarks_store(book["hash"]))) == len(positions), \
            "the second sync changed the local bookmarks"

    adds = bookmark_adds(verify_device(manifest, "bmsamechapter"), book["hash"])
    kept = {a["pos"] for a in adds}
    assert all(pos in kept for pos in positions), \
        f"bookmarks deleted on the server by the second sync: {sorted(set(positions) - kept)}"


def scenario_bookmark_recover_lost() -> None:
    """A copy left by the builds before #120's fix gets its lost bookmarks back instead of
    deleting them on the server. Those builds acknowledged three bookmarks of one chapter in the
    same second, kept only the last one, at the chapter's start, and wrote a BOB1 store: its
    complete key set would now delete the two others on the server. The first sync asks for
    everything again (sidecarState new); the bookmark this device made must not come back twice."""
    manifest, books = load_seed()
    book = books[FRESH_BOOK["bookmark_recover_lost"]]
    now = int(time.time())
    received = [f"/body/DocFragment[2]/body/p[{p}]/text().0" for p in (2, 6, 10)]
    own = "/body/DocFragment[4]/body/p[3]/text().0"
    reset_sim_bookmarks(manifest, book["hash"])
    add_peer_bookmarks(manifest, book["hash"], received, now)

    # The old build, as the simulator's device: its own bookmark went up, then the three came
    # down and were acknowledged with identities minted in one second.
    sim = sim_device(manifest, auth=True)
    own_epoch, applied_epoch = now - 1200, now - 600
    sim.exchange_bookmarks(book["hash"], keys=[], keys_complete=False,
                           changes=[{"datetime": kodatetime(own_epoch), "pos": own}])
    offered = [a for a in bookmark_adds(sim, book["hash"]) if a["pos"] in received]
    assert sorted(a["pos"] for a in offered) == sorted(received), f"setup: not offered to the device: {offered}"
    applied_dt = kodatetime(applied_epoch)
    sim.exchange_bookmarks_ack(book["hash"], applied=[
        {"serverId": a["serverId"], "status": "applied", "key": bookmark_key(applied_dt, a["pos"]),
         "datetime": applied_dt, "pos": a["pos"]} for a in offered])

    with sd_card(manifest) as fs:
        sim_path = fs.open_book(book)
        store = fs.bookmark_sync_store(book["hash"])
        survivor = received[-1]
        write_bookmark_store(fs.bookmarks_store(book["hash"]), "", "", sim_path, [
            {"spine": 3, "progress": 0.5, "timestamp": own_epoch, "paragraph": 3, "snippet": "Own bookmark"},
            {"spine": 1, "progress": 0.0, "timestamp": applied_epoch, "paragraph": 10, "snippet": "Bookmark"},
        ])
        write_bob_store(store, applied_epoch, [
            {"timestamp": own_epoch, "identityEpoch": own_epoch, "spine": 3, "pos": own},
            {"timestamp": applied_epoch, "identityEpoch": applied_epoch, "spine": 1, "pos": survivor},
        ], magic=b"BOB1")

        sync(fs, "apply", what="first sync")
        assert bob_store_magic(store) == b"BOB2", f"the store was not moved on: {bob_store_magic(store)}"
        bookmarks = read_bookmark_store(fs.bookmarks_store(book["hash"]))
        chapter = sorted((b for b in bookmarks if b["spine"] == 1), key=lambda b: b["progress"])
        assert [b["paragraph"] for b in chapter] == [2, 6, 10], f"the lost bookmarks did not come back: {bookmarks}"
        assert all(b["progress"] > 0.0 for b in chapter), f"a bookmark still sits at its chapter's start: {chapter}"
        assert sum(1 for b in bookmarks if b["spine"] == 3) == 1, f"this device's bookmark came back twice: {bookmarks}"
        _, records = read_bob_store(store)
        assert sorted(r["pos"] for r in records) == sorted(received + [own]), f"records: {records}"
        assert len({r["timestamp"] for r in records}) == len(records), f"records share a timestamp: {records}"

        sync(fs, "apply", what="second sync")
        assert len(read_bookmark_store(fs.bookmarks_store(book["hash"]))) == len(bookmarks), \
            "the second sync changed the local bookmarks"

    kept = {a["pos"] for a in bookmark_adds(verify_device(manifest, "bmrecover"), book["hash"])}
    assert all(pos in kept for pos in received + [own]), \
        f"bookmarks deleted on the server: {sorted(set(received + [own]) - kept)}"
