"""Outdated copies and book updates: a copy of a book BookOrbit has since rewritten holds
its changes back instead of losing them, the sync offers the update, and the highlights
follow the new copy, except those in a chapter the copies disagree on."""

from __future__ import annotations

import posixpath
import re
import struct
import time
import xml.etree.ElementTree as ET
import zipfile
from pathlib import Path

from formats import (local_highlight, read_boa_store, read_bob_store, read_clipping_store, write_bob_store,
                     write_bookmark_store)
from kosync import partial_md5
from server import FRESH_BOOK, annotation_adds, load_seed, rewrite_book_file, verify_device
from sim import LIBRARY, POWER_PRESS, run_simulator, sd_card, sync


def rewritten_on_server(manifest: dict, book: dict, tag: str) -> bool:
    """Makes BookOrbit rewrite the book's file, which leaves the library's original copy
    outdated, and tells whether the server now answers that copy positionIncompatible.
    Servers before 3.6.0 do not, and the scenario is then skipped."""
    rewrite_book_file(manifest, book)
    reply = verify_device(manifest, tag).exchange_annotations(book["hash"], keys=[], keys_complete=False, changes=[])
    if book["hash"] in reply.get("positionIncompatible", []):
        return True
    print("  (skipped: this server does not answer positionIncompatible)")
    return False


def epub_spine_entries(path: Path) -> list[str]:
    """The ZIP entry names an EPUB's spine reads, in reading order."""
    with zipfile.ZipFile(path) as z:
        container = ET.fromstring(z.read("META-INF/container.xml"))
        opf_path = next(e.get("full-path") for e in container.iter() if e.tag.endswith("rootfile"))
        opf = ET.fromstring(z.read(opf_path))
    base = posixpath.dirname(opf_path)
    manifest = {e.get("id"): e.get("href") for e in opf.iter() if e.tag.endswith("}item")}
    return [posixpath.normpath(posixpath.join(base, manifest[e.get("idref")]))
            for e in opf.iter() if e.tag.endswith("}itemref")]


def write_older_copy(source: Path, dest: Path, changed_entry: str) -> None:
    """Another copy of the same book, as an older file of it: one chapter's markup differs
    (its CRC-32 with it), every other entry is byte-identical. The mimetype stays first and
    stored, as the EPUB container requires."""
    with zipfile.ZipFile(source) as src, zipfile.ZipFile(dest, "w") as out:
        for info in src.infolist():
            data = src.read(info.filename)
            if info.filename == changed_entry:
                data = data.replace(b"</body>", b"<p>A paragraph only the older copy had.</p></body>", 1)
            out.writestr(info, data, compress_type=info.compress_type)


def scenario_outdated_copy_holds_changes() -> None:
    """The device holds the library's original copy of a book whose file BookOrbit has since
    rewritten (`write-and-rename`). BookOrbit 3.6.0+ answers both exchanges with
    positionIncompatible, HTTP 200 and no result, and keeps nothing: the device must not count
    its highlight or bookmark as sent (v1.6.1+bookorbit.7 did, and never sent them again; both
    upload watermarks stay put), and the sync still ends normally."""
    manifest, books = load_seed()
    book = books[FRESH_BOOK["outdated_copy"]]
    if not rewritten_on_server(manifest, book, "outdated"):
        return
    now = int(time.time())
    text = f"Outdated copy highlight {now}"
    bookmark_pos = "/body/DocFragment[3]/body/p[1]/text().0"

    with sd_card(manifest) as fs:
        sim_path = fs.open_book(book)
        fs.write_local_highlights(book, sim_path, [local_highlight(text, 4545, now)])
        write_bookmark_store(fs.bookmarks_store(book["hash"]), book.get("title", ""),
                             book.get("author", ""), sim_path,
                             [{"spine": 2, "progress": 0.25, "timestamp": now,
                               "chapter": "Chapter", "paragraph": 1, "snippet": "Outdated copy bookmark"}])
        write_bob_store(fs.bookmark_sync_store(book["hash"]), 0, [{
            "timestamp": now, "identityEpoch": now, "spine": 2, "pos": bookmark_pos,
        }])

        output = sync(fs, "upload")
        assert re.search(r"Highlight exchange result=0 \(http=20\d, unmatched=1, incompatible=1", output), \
            "the highlight exchange did not take the reply for an outdated copy"
        assert re.search(r"Bookmark upload result=0 \(http=20\d, unmatched=1, incompatible=1", output), \
            "the bookmark exchange did not take the reply for an outdated copy"
        highlight_watermark, records = read_boa_store(fs.annotation_store(book["hash"]))
        assert highlight_watermark < now and records, \
            f"the highlight was counted as sent (watermark {highlight_watermark})"
        bookmark_watermark, _ = read_bob_store(fs.bookmark_sync_store(book["hash"]))
        assert bookmark_watermark < now, f"the bookmark was counted as sent (watermark {bookmark_watermark})"


def scenario_book_update_carries_state() -> None:
    """A catalog re-download replaced a book's copy by the library's, and the device was reset
    before the copy it replaced was carried across (it still waits at <book>.old). The next sync
    of the book finishes it first, here one started from Home, which loads the book without the
    reader: the highlights and BookOrbit state move from the old copy's hash to the new one's,
    the highlight in the chapter the copies disagree on is orphaned (not sent), the one in an
    unchanged chapter is sent (the upload watermark rewound), and the old copy is gone.
    (book_update_offer covers the reader.)"""
    manifest, books = load_seed()
    book = books[FRESH_BOOK["book_update"]]
    source = LIBRARY / book["file"]
    spine = epub_spine_entries(source)
    assert len(spine) >= 3, f"{book['file']} has too few chapters for this scenario"
    now = int(time.time())
    kept_text = f"Kept highlight {now}"
    orphan_text = f"Orphaned highlight {now}"

    with sd_card(manifest) as fs:
        sim_path = fs.open_book(book)
        old_copy = fs.path(sim_path + ".old")
        write_older_copy(source, old_copy, spine[2])
        old_hash = partial_md5(old_copy)
        assert old_hash != book["hash"], "the older copy did not get a hash of its own"

        # Watermark past both: what v1.6.1+bookorbit.7 left after an exchange it misread.
        fs.write_local_highlights(book, sim_path, [local_highlight(kept_text, 4646, now, spine=1, paragraph=1),
                                                   local_highlight(orphan_text, 4747, now, spine=2, paragraph=1)],
                                  watermark=now + 60, book_hash=old_hash)

        sync(fs, "upload", timeout_s=120)

        assert not old_copy.exists(), "the replaced copy was left on the card"
        assert not fs.clippings_store(old_hash).exists(), "the highlights stayed with the old copy"
        moved = {c["text"] for c in read_clipping_store(fs.clippings_store(book["hash"]))}
        assert {kept_text, orphan_text} <= moved, f"highlights missing from the new copy: {moved}"
        state = fs.state_dir(book["hash"])
        assert not (state / "replaced_chapters.bin").exists(), "the chapter comparison never ran"
        orphans = (state / "orphaned_clippings.bin").read_bytes()
        assert orphans[:4] == b"OCL1" and struct.unpack_from("<H", orphans, 4)[0] == 1, \
            f"expected exactly one orphaned highlight: {orphans!r}"
        assert struct.unpack_from("<IHH", orphans, 6) == (4747, 2, 1), "the wrong highlight was orphaned"
        watermark, _ = read_boa_store(fs.annotation_store(book["hash"]))
        assert now <= watermark < now + 60, f"the watermark was not rewound and advanced: {watermark}"

    texts = {a["text"] for a in annotation_adds(verify_device(manifest, "bkupd"), book["hash"])}
    assert kept_text in texts, f"the highlight in an unchanged chapter was not sent: {texts}"
    assert orphan_text not in texts, "the orphaned highlight was sent"


def scenario_book_update_offer() -> None:
    """The whole update, as a user takes it. BookOrbit rewrites a book's file; the device,
    holding the library's original copy (copied to the card: the download index does not know
    it), syncs a highlight. The sync takes the positionIncompatible reply, finds the book through
    the match check, and offers the update at its end; the scripted run accepts it
    (CROSSINK_SIM_BOOKORBIT_UPDATE). The catalog downloads the library's file over the copy and
    reopens the book, whose highlight followed it to the new copy's hash: the chapter comparison
    ran and orphaned nothing. A second sync then sends that highlight."""
    manifest, books = load_seed()
    book = books[FRESH_BOOK["book_update_offer"]]
    if not rewritten_on_server(manifest, book, "updoffer"):
        return
    now = int(time.time())
    text = f"Highlight carried by the update {now}"

    with sd_card(manifest) as fs:
        sim_path = fs.open_book(book)
        fs.write_local_highlights(book, sim_path, [local_highlight(text, 5151, now)])

        # Sync, offer, catalog download and the reader reopening the book: nothing ends that
        # chain, so the run stops on its timeout and the card tells how far it went.
        output = run_simulator(fs, POWER_PRESS, choice="upload", timeout_s=75,
                               extra_env={"CROSSINK_SIM_BOOKORBIT_UPDATE": "1"})
        assert "Restarting into the catalog to update" in output, "the sync did not offer the update"
        assert "Downloading file" in output, "the catalog did not download the library's copy"
        book_file = fs.path(sim_path)
        new_hash = partial_md5(book_file)
        assert new_hash != book["hash"], "the copy on the card was not replaced"
        assert not Path(str(book_file) + ".old").exists(), "the replaced copy was left on the card"
        assert not fs.clippings_store(book["hash"]).exists(), "the highlight stayed with the old copy"
        moved = {c["text"] for c in read_clipping_store(fs.clippings_store(new_hash))}
        assert text in moved, f"the highlight did not follow the update: {moved}"
        assert not (fs.state_dir(new_hash) / "replaced_chapters.bin").exists(), \
            "the reopened book never compared its chapters"
        assert not (fs.state_dir(new_hash) / "orphaned_clippings.bin").exists(), \
            "a metadata write-back changed no chapter, yet a highlight was orphaned"

        output = sync(fs, "upload", what="the second sync")
        assert "incompatible=1" not in output, "the updated copy is still outdated for the server"

    texts = {a["text"] for a in annotation_adds(verify_device(manifest, "updoffer2"), new_hash)}
    assert text in texts, f"the highlight was not sent after the update: {texts}"
