"""Highlights: the exchange with the server both ways and in batches, the deletions a
complete key set carries (and the guard when that set is lost), the copies the server
offers back, and the positions the reader mints for highlights saved without one."""

from __future__ import annotations

import random
import re
import time

from formats import local_highlight, read_boa_store, read_clipping_store, write_boa_store, write_clipping_store
from kosync import KosyncDevice, kodatetime
from server import (FRESH_BOOK, annotation_adds, load_seed, offered_annotation_texts, peer_device,
                    server_accepts_sidecar_state, sim_device, verify_device)
from sim import run_simulator, sd_card, sync


def server_highlight(text: str, epoch: int, paragraph: int = 2, **fields) -> dict:
    """A highlight as another device sends it, from the start of a paragraph of chapter 3."""
    xpath = f"/body/DocFragment[3]/body/p[{paragraph}]/text()"
    return {"datetime": kodatetime(epoch), "pos0": f"{xpath}.0", "pos1": f"{xpath}.{len(text)}",
            "text": text, "chapter": "Chapter", **fields}


def add_peer_highlights(manifest: dict, book: dict, *changes: dict) -> KosyncDevice:
    """Highlights the synthetic peer makes on the book; returns the peer."""
    peer = peer_device(manifest)
    peer.exchange_annotations(book["hash"], keys=[], keys_complete=False, changes=list(changes))
    return peer


def scenario_highlight_pull() -> None:
    """A highlight created by another device reaches the simulator's clipping
    store, and its position is recorded for future syncs. A fresh peer highlight
    per run keeps this repeatable: the simulator's device id has already
    acknowledged older ones on previous runs."""
    manifest, _ = load_seed()
    book = manifest["highlights"][0]
    now = int(time.time())
    text = f"Peer highlight {now}"
    add_peer_highlights(manifest, book, server_highlight(text, now))

    with sd_card(manifest) as fs:
        fs.open_book(book)
        sync(fs, "apply")

        clippings = read_clipping_store(fs.clippings_store(book["hash"]))
        mine = [c for c in clippings if c["text"] == text]
        assert mine, f"peer highlight missing locally; texts={[c['text'] for c in clippings]}"
        assert mine[0]["spine"] == 2, f"highlight landed in the wrong chapter: {mine[0]}"
        _, records = read_boa_store(fs.annotation_store(book["hash"]))
        assert any(r["pos0"].startswith("/body/DocFragment[3]") for r in records), \
            f"no position record for the received highlight: {records}"


def scenario_highlight_push() -> None:
    """A local highlight (clipping + minted position record, exactly what the
    reader persists on creation) reaches the server and is offered to a device
    that has never seen it."""
    manifest, books = load_seed()
    book = books[FRESH_BOOK["highlight_push"]]
    now = int(time.time())
    text = f"Local highlight {now}"

    with sd_card(manifest) as fs:
        sim_path = fs.open_book(book)
        fs.write_local_highlights(book, sim_path, [local_highlight(text, 4242, now)])
        sync(fs, "upload")
        watermark, _ = read_boa_store(fs.annotation_store(book["hash"]))
        assert watermark >= now, f"upload watermark did not advance: {watermark} < {now}"

    adds = annotation_adds(verify_device(manifest, "hlpush"), book["hash"])
    assert any(a["text"] == text for a in adds), \
        f"pushed highlight not on server; offered texts={[a['text'] for a in adds]}"


def scenario_highlight_push_backlog() -> None:
    """More highlights than one upload batch reach the server in a single sync (batches
    of 8 on one session), and the watermark ends on the newest. The store lists them out
    of datetime order, as a clock set back between two highlights leaves it: batches taken
    in file order would move the watermark past highlights not yet sent, and those would
    never go."""
    manifest, books = load_seed()
    book = books[FRESH_BOOK["highlight_push_backlog"]]
    now = int(time.time())
    count = 20  # two full batches of 8, then a partial one
    highlights = []
    for i in range(count):
        spine, paragraph = (1, i + 1) if i < 10 else (2, i - 9)
        # 7 and 20 are coprime, so the dates are a permutation of the last 20 seconds
        highlights.append(local_highlight(f"Backlog highlight {now} #{i}", 5000 + i, now - count + (i * 7) % count,
                                          spine, paragraph, length=10))

    with sd_card(manifest) as fs:
        sim_path = fs.open_book(book)
        fs.write_local_highlights(book, sim_path, highlights)

        output = sync(fs, "upload", timeout_s=120)
        assert f"Highlight batch accepted: {count} of {count} sent" in output, \
            "the sync did not send the whole backlog"
        assert f", sent +{count} -0" in output, \
            "the summary does not count what was sent"
        watermark, _ = read_boa_store(fs.annotation_store(book["hash"]))
        newest = max(record["identityEpoch"] for _, record in highlights)
        assert watermark == newest, f"watermark {watermark} is not the newest highlight {newest}"

    offered = offered_annotation_texts(verify_device(manifest, "hlbacklog"), book["hash"])
    missing = [clipping["text"] for clipping, _ in highlights if clipping["text"] not in offered]
    assert not missing, f"{len(missing)} of {count} highlights not on the server: {missing}"


def scenario_highlight_pull_backlog() -> None:
    """More server highlights than one batch reach the device in a single sync: each
    acknowledged batch of 8 makes room for the next pull on the same session. Five share
    each paragraph, so several land in one paragraph within the same second; each needs a
    clipping timestamp of its own there, or it would take another's position record."""
    manifest, books = load_seed()
    book = books[FRESH_BOOK["highlight_pull_backlog"]]
    now = int(time.time())
    count = 20  # two full batches of 8, then a partial one
    texts = [f"Pulled backlog {now} #{i}" for i in range(count)]
    add_peer_highlights(manifest, book, *(server_highlight(text, now - count + i, paragraph=i % 4 + 1)
                                          for i, text in enumerate(texts)))

    with sd_card(manifest) as fs:
        fs.open_book(book)
        output = sync(fs, "upload", timeout_s=120)
        clippings = read_clipping_store(fs.clippings_store(book["hash"]))
        # What landed, not `count`: a server that ran this scenario before offers its earlier
        # highlights again to the fresh device copy, and they land too.
        assert f"Highlights received +{len(clippings)} -0, sent +0 -0" in output, \
            "the summary does not count what was received"
        missing = [t for t in texts if t not in {c["text"] for c in clippings}]
        assert not missing, f"{len(missing)} of {count} server highlights not on the device: {missing}"
        refs = [(c["timestamp"], c["spine"], c["paragraph"]) for c in clippings]
        assert len(set(refs)) == len(refs), f"two clippings share a timestamp in one paragraph: {refs}"
        _, records = read_boa_store(fs.annotation_store(book["hash"]))
        assert len(records) == len(clippings), \
            f"{len(clippings)} clippings but {len(records)} position records"


def scenario_highlight_delete_propagates() -> None:
    """Deleting a synced highlight locally deletes it on the server: the second
    sync's complete key set no longer names it, and its summary counts the deletion.
    Two simulator runs on one SD."""
    manifest, books = load_seed()
    book = books[FRESH_BOOK["highlight_delete"]]
    now = int(time.time())
    text = f"Doomed highlight {now}"
    add_peer_highlights(manifest, book, server_highlight(text, now))

    with sd_card(manifest) as fs:
        sim_path = fs.open_book(book)
        sync(fs, "apply", what="first sync")
        clippings = read_clipping_store(fs.clippings_store(book["hash"]))
        assert any(c["text"] == text for c in clippings), "setup: highlight never arrived"

        # The user deletes the highlight: the clipping goes, the position record
        # stays behind (the next sync drops the orphan and reports the shrunken set).
        write_clipping_store(fs.clippings_store(book["hash"]), "", "", sim_path,
                             [c for c in clippings if c["text"] != text])

        output = sync(fs, "apply", what="second sync")
        assert "Highlights received +0 -0, sent +0 -1" in output, \
            "the summary does not count the deletion the server applied"

    adds = annotation_adds(verify_device(manifest, "hldel"), book["hash"])
    assert not any(a["text"] == text for a in adds), \
        f"deleted highlight still on server: {[a['text'] for a in adds]}"


def scenario_highlight_delete_propagates_large() -> None:
    """A book with more highlights than the 64 a key set used to be held to (80 here)
    still tells the server what was deleted on the device. The key set is written straight
    into the request body; built as JSON nodes it did not fit beside the TLS session, so it
    was never sent and a deletion on such a book never reached the server."""
    manifest, books = load_seed()
    book = books[FRESH_BOOK["highlight_delete_propagates_large"]]
    now = int(time.time())
    count = 80
    # The server knows a book's highlights by their datetime alone (KOReader's identity), so
    # a rerun inside `count` seconds of the previous one, dated now - count + i, would land on
    # that run's datetimes and its highlights would merge into the old ones. A random span in
    # the past keeps runs apart.
    first_epoch = now - random.randrange(86400, 365 * 86400)
    highlights = [local_highlight(f"Large book highlight {now} #{i}", 7000 + i, first_epoch + i,
                                  1 + i // 20, 1 + i % 20, length=10) for i in range(count)]
    doomed = {highlights[3][0]["text"], highlights[41][0]["text"]}

    with sd_card(manifest) as fs:
        sim_path = fs.open_book(book)
        store = fs.clippings_store(book["hash"])
        fs.write_local_highlights(book, sim_path, highlights)

        output = sync(fs, "upload", timeout_s=150, what="first sync")
        assert f"Highlight batch accepted: {count} of {count} sent" in output, \
            "setup: the highlights did not all reach the server"
        assert f"keys={count}," in output, "the first sync did not send its whole key set"

        # Two deleted on the device: their position records stay behind, and the next sync
        # drops them and reports the shrunken set. Start from the store as the sync left it: on
        # a server a previous run already used, the first sync also received that run's
        # highlights, and dropping them here would be deleting them too.
        held = read_clipping_store(store)
        write_clipping_store(store, book.get("title", ""), book.get("author", ""), sim_path,
                             [c for c in held if c["text"] not in doomed])

        output = sync(fs, "upload", what="second sync")
        assert f"keys={len(held) - 2}," in output, "the second sync did not send its whole key set"
        assert "deleted on server=2)" in output, "the server did not delete the two highlights"
        # Only the sent half is this scenario's: on a used server the book can hold more than
        # the device's 256, and the two freed slots are filled from the server's surplus.
        assert re.search(r"Highlights received \+\d+ -0, sent \+0 -2\b", output), \
            "the summary does not count the deletions the server applied"

    offered = offered_annotation_texts(verify_device(manifest, "hldellarge"), book["hash"])
    kept = {clipping["text"] for clipping, _ in highlights} - doomed
    assert not doomed & offered, f"deleted highlights still on the server: {sorted(doomed & offered)}"
    missing = kept - offered
    assert not missing, f"{len(missing)} kept highlights missing from the server: {sorted(missing)[:5]}"


def scenario_highlight_delete_guard() -> None:
    """A lost sync-state store must NOT read as 'every highlight deleted': with
    the store gone, the sync reports no key set and the server keeps everything.
    This is the destructive-wipe guard in BookOrbitHighlightSync::prepare."""
    manifest, books = load_seed()
    book = books[FRESH_BOOK["highlight_delete_guard"]]
    now = int(time.time())
    text = f"Survivor highlight {now}"
    peer = add_peer_highlights(manifest, book, server_highlight(text, now))

    with sd_card(manifest) as fs:
        sim_path = fs.open_book(book)
        sync(fs, "apply", what="first sync")
        clippings = read_clipping_store(fs.clippings_store(book["hash"]))
        assert any(c["text"] == text for c in clippings), "setup: highlight never arrived"

        # History lost (wiped card, fresh device) AND the clippings gone: without
        # the guard, the empty-complete key set would erase the server's copy.
        fs.annotation_store(book["hash"]).unlink()
        write_clipping_store(fs.clippings_store(book["hash"]), "", "", sim_path, [])

        output = sync(fs, "apply", what="second sync")
        assert "No highlight sync history" in output, \
            "guard log missing: deletions may have been reported over a lost store"
        # Nothing left on the device: the "new" sidecar state asks BookOrbit (3.5.0 and
        # later) to offer back everything this copy had, and it does.
        if server_accepts_sidecar_state(peer):
            clippings = read_clipping_store(fs.clippings_store(book["hash"]))
            assert any(c["text"] == text for c in clippings), \
                f"the lost highlight was not offered back; texts={[c['text'] for c in clippings]}"

    adds = annotation_adds(verify_device(manifest, "hlguard"), book["hash"])
    assert any(a["text"] == text for a in adds), \
        "server highlight was erased despite the lost local store"


def scenario_highlight_delete_all_propagates() -> None:
    """Deleting every synced highlight of a book deletes them on the server. BookOrbit
    3.5.0 reads a complete key set that keeps none of a device's highlights (two at
    least) as a lost copy and offers them all back, unless the device says its copy is
    "known". These highlights all came from the server, so the store their first sync
    started must already count as exchanged."""
    manifest, books = load_seed()
    book = books[FRESH_BOOK["highlight_delete_all"]]
    now = int(time.time())
    texts = [f"Doomed pair {i} {now}" for i in (1, 2)]
    add_peer_highlights(manifest, book, *(server_highlight(text, now + i, paragraph=i + 2)
                                          for i, text in enumerate(texts)))

    with sd_card(manifest) as fs:
        sim_path = fs.open_book(book)
        sync(fs, "apply", what="first sync")
        clippings = read_clipping_store(fs.clippings_store(book["hash"]))
        assert all(any(c["text"] == t for c in clippings) for t in texts), "setup: highlights never arrived"

        write_clipping_store(fs.clippings_store(book["hash"]), "", "", sim_path,
                             [c for c in clippings if c["text"] not in texts])

        sync(fs, "apply", what="second sync")
        clippings = read_clipping_store(fs.clippings_store(book["hash"]))
        assert not any(c["text"] in texts for c in clippings), "the deleted highlights came back"

    adds = annotation_adds(verify_device(manifest, "hldelall"), book["hash"])
    assert not any(a["text"] in texts for a in adds), \
        f"deleted highlights still on server: {[a['text'] for a in adds]}"


def scenario_highlight_reoffer_no_duplicate() -> None:
    """The device's own highlight offered back as new is recognised, not added twice.
    BookOrbit 3.5.0 keeps sync state per device and book file, so the first exchange
    after a server upgrade offers every highlight of a book back like this; here the
    simulator's state is reset by sending "new" in its name. The text is longer than
    the device keeps of an incoming highlight, so only the identity can match it."""
    manifest, books = load_seed()
    book = books[FRESH_BOOK["highlight_reoffer"]]
    sim = sim_device(manifest, auth=True)
    if not server_accepts_sidecar_state(sim):
        print("  (skipped: this server keeps state per device and cannot be asked to re-offer)")
        return
    now = int(time.time())
    text = f"Long local highlight {now} " + "lorem ipsum dolor " * 140  # ~2.5 KB, past the 2 KB cut

    with sd_card(manifest) as fs:
        sim_path = fs.open_book(book)
        fs.write_local_highlights(book, sim_path, [local_highlight(text, 4343, now, paragraph=3)])
        sync(fs, "upload", what="first sync")

        sim.exchange_annotations(book["hash"], keys=[], keys_complete=False, changes=[], sidecar_state="new")

        sync(fs, "upload", what="second sync")
        mine = [c for c in read_clipping_store(fs.clippings_store(book["hash"]))
                if c["text"].startswith(f"Long local highlight {now} ")]
        assert len(mine) == 1, f"the re-offered highlight was added again: {len(mine)} copies"


def scenario_highlight_note_survives() -> None:
    """A highlight received from the server is never sent back: it carries a note this
    device does not show, and BookOrbit 3.5.0 merges an upload field by field, so the
    missing note would read as deleted. The watermark is rewound after the first sync,
    the state in which such a highlight used to be re-sent (a local highlight waiting
    to upload kept the watermark from covering it)."""
    manifest, books = load_seed()
    book = books[FRESH_BOOK["highlight_note"]]
    now = int(time.time())
    text = f"Annotated highlight {now}"
    note = f"Peer note {now}"
    add_peer_highlights(manifest, book, server_highlight(text, now, note=note))

    with sd_card(manifest) as fs:
        fs.open_book(book)
        sync(fs, "apply", what="first sync")
        clippings = read_clipping_store(fs.clippings_store(book["hash"]))
        assert any(c["text"] == text for c in clippings), "setup: highlight never arrived"

        store = fs.annotation_store(book["hash"])
        _, records = read_boa_store(store)
        write_boa_store(store, 0, records)

        sync(fs, "apply", what="second sync")

    adds = annotation_adds(verify_device(manifest, "hlnote"), book["hash"])
    mine = [a for a in adds if a["text"] == text]
    assert mine, f"highlight missing on the server; texts={[a['text'] for a in adds]}"
    assert mine[0].get("note") == note, f"the server lost the note: {mine[0]}"


# Chapter 5 (spine 4) of the library template, test_reader_rendering_matrix.epub: highlights
# in its heading and first list item sit outside any <p>, the markup of issue #84 (a chapter
# made of <blockquote> text). The heading precedes every <p>, so the reader's paragraph hint
# for it is 0, which the resolver used to reject outright: such a highlight never synced. The
# note <p> is the control, whose position must stay exactly what the <p>-only resolver minted.
OUTSIDE_PARAGRAPH_SPINE = 4
OUTSIDE_PARAGRAPH_HIGHLIGHTS = [
    {"text": "Blocks and Edge Cases", "paragraph": 0, "node": "h1[1]"},
    {"text": "Ordered list item with enough text to wrap and check number alignment.",
     "paragraph": 1, "node": "ol[1]/li[1]"},
    {"text": "PASS when block-level spacing is stable and no block overlaps the next one.",
     "paragraph": 1, "node": "p[1]"},
]


def scenario_highlight_mint_outside_paragraph() -> None:
    """Highlights the device saved but never positioned (what firmware before the #84
    fix left on the card: a clipping and no position record) get minted by the reader
    and reach the server at their precise text node, heading and list item included.

    The reader positions one highlight per visit to a chapter, so the card goes through
    one wake into the chapter per highlight; the last wake also syncs. Clippings are
    stored in the order they must be minted, and a highlight the resolver cannot
    position is retried first on every visit, so with the old resolver the heading
    starves the other two and the scenario fails on all three."""
    manifest, books = load_seed()
    book = books[FRESH_BOOK["highlight_mint_outside_paragraph"]]
    fragment = f"/body/DocFragment[{OUTSIDE_PARAGRAPH_SPINE + 1}]/body"
    expected = {h["text"]: (f"{fragment}/{h['node']}/text()[1].0",
                            f"{fragment}/{h['node']}/text()[1].{len(h['text'])}")
                for h in OUTSIDE_PARAGRAPH_HIGHLIGHTS}

    with sd_card(manifest) as fs:
        sim_path = fs.open_book(book, progress=(OUTSIDE_PARAGRAPH_SPINE, 0, 1))
        write_clipping_store(fs.clippings_store(book["hash"]), book.get("title", ""),
                             book.get("author", ""), sim_path,
                             [{"spine": OUTSIDE_PARAGRAPH_SPINE, "paragraph": h["paragraph"],
                               "timestamp": 5000 + i, "text": h["text"], "chapter": "Blocks and Edge Cases",
                               "wordCount": len(h["text"].split())}
                              for i, h in enumerate(OUTSIDE_PARAGRAPH_HIGHLIGHTS)])
        store = fs.annotation_store(book["hash"])

        for visit in range(len(OUTSIDE_PARAGRAPH_HIGHLIGHTS)):
            last = visit == len(OUTSIDE_PARAGRAPH_HIGHLIGHTS) - 1
            fs.set_asleep_in_book(sim_path)
            # A short POWER press in the reader starts BookOrbit Sync, which uploads (and
            # silently reboots into the sync); earlier visits only let the reader mint.
            output = run_simulator(fs, input_script="8000:POWER:120" if last else "8000:QUIT",
                                   choice="upload", timeout_s=90 if last else 30, wake_into_reader=True)
            assert "Entering activity: EpubReader" in output, f"visit {visit + 1} never opened the reader"
            _, records = read_boa_store(store)
            assert len(records) == visit + 1, \
                f"visit {visit + 1} left {len(records)} positioned highlights: {records}"
        assert "sync scenario finished" in output, "sync never reached its end marker"

        _, records = read_boa_store(store)
        by_timestamp = {r["timestamp"]: r for r in records}
        for i, h in enumerate(OUTSIDE_PARAGRAPH_HIGHLIGHTS):
            record = by_timestamp.get(5000 + i)
            assert record, f"no position record for \"{h['text']}\": {records}"
            assert (record["pos0"], record["pos1"]) == expected[h["text"]], \
                f"\"{h['text']}\" positioned at {record['pos0']} .. {record['pos1']}"
        clipped = {c["text"] for c in read_clipping_store(fs.clippings_store(book["hash"]))}
        assert clipped == set(expected), f"clipping texts changed: {clipped}"

    adds = annotation_adds(verify_device(manifest, "hlmint"), book["hash"])
    for text, (pos0, pos1) in expected.items():
        mine = [a for a in adds if a["text"] == text]
        assert mine, f"\"{text}\" not on server; offered texts={[a['text'] for a in adds]}"
        assert (mine[-1]["pos0"], mine[-1]["pos1"]) == (pos0, pos1), \
            f"\"{text}\" stored at {mine[-1]['pos0']} .. {mine[-1]['pos1']} on the server"
