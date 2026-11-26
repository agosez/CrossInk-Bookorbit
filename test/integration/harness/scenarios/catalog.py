"""The BookOrbit catalog: browsing its sections and listings, which EPUBs it lists, sorts,
filters and list sections, long listings kept to a window of pages, and downloads with
the book actions around them.

Browsing never silent-reboots before exit, so input scripts drive it end to end and the
scenarios assert through the list cache the browser writes for each screen it loaded. A
download does restart first: the simulator's X4 profile has no PSRAM, so like an X3/X4
(issue #96) it downloads from a network boot of its own, which the second script drives.
"""

from __future__ import annotations

import json
import re
from pathlib import Path

from formats import book_listings, listings_by_key, read_progress_bin, write_progress_bin
from server import DROP, load_seed, server_cutting_downloads, sim_device, stall, want_to_read_epubs
from sim import CATALOG_ROOT, TO_CATALOG, SimFs, presses, run_simulator, sd_card

BOOT_TARGET_BOOKORBIT_DOWNLOAD = 34  # NetworkBootTarget::BOOKORBIT_DOWNLOAD, src/SilentRestart.h

# The catalog root's rows, from the top: Continue reading, Recently added, Libraries,
# Collections, SmartScopes, ... and All books, the 8th.
TO_COLLECTIONS = TO_CATALOG + presses(11000, "DOWN", 3) + ["12100:CONFIRM"]
TO_ALL_BOOKS = TO_CATALOG + presses(11000, "DOWN", 7) + ["13450:CONFIRM"]

LISTING_PAGE_LOG = re.compile(r"Listing page (\d+) from (cache|server): (\d+) rows in memory from row (\d+)")


def scenario_catalog_collections_browse() -> None:
    """The catalog browser lists the server's Collections section and the books
    inside one. Browse-only: nothing mutates server-side, so this is repeatable as-is."""
    manifest, _ = load_seed()
    collection = manifest["collection"]

    with sd_card(manifest) as fs:  # no open book: boots to home
        script = ";".join([*TO_COLLECTIONS, "14500:CONFIRM", "17500:QUIT"])  # the seeded collection
        run_simulator(fs, input_script=script, choice="apply", timeout_s=60)

        caches = fs.list_caches()
        roots = [c for c in caches if "sections" in c]
        assert roots and any(s["id"] == "collections" for s in roots[0]["sections"]), \
            f"Collections missing from the cached root: {roots}"
        facets = [c for c in caches if "hasNext" in c]
        assert any(i["title"] == collection["name"] for c in facets for i in c["items"]), \
            f"collection list never loaded: {facets}"
        books = book_listings(caches)
        assert books, "collection books were never listed"
        listed = {b["title"] for b in books[0]["items"]}
        expected = {Path(b["file"]).stem for b in collection["books"]}
        assert listed == expected, f"collection books mismatch: {listed} != {expected}"


def scenario_catalog_empty_listing_back() -> None:
    """An empty listing (a collection with no books) shows the no-entries error,
    and Back must climb out of it — the regression had the error screen's Back
    reload the very listing it was showing, forever. Proven by navigating onward
    after the error: Back lands on the row it left, the empty collection, and the
    non-empty collection above it still opens with its books."""
    manifest, _ = load_seed()
    collection = manifest["collection"]

    with sd_card(manifest) as fs:  # no open book: boots to home
        script = ";".join([
            *TO_COLLECTIONS,
            "14500:DOWN", "14900:CONFIRM",                # Zero Shelf (empty) -> error screen
            "17500:BACK",                                 # must climb back to the collection list,
            "19100:UP",                                   #   on Zero Shelf's row
            "19500:CONFIRM",                              # Integration Shelf -> its books
            "22500:QUIT",
        ])
        run_simulator(fs, input_script=script, choice="apply", timeout_s=60)

        books = book_listings(fs.list_caches())
        assert any(c["total"] == 0 for c in books), "the empty collection was never opened"
        expected = {Path(b["file"]).stem for b in collection["books"]}
        assert any({i["title"] for i in c["items"]} == expected for c in books), \
            "navigation after the empty-listing error never reached the collection's books"


def scenario_catalog_libraries_browse() -> None:
    """Libraries is a browsable root section, like BookOrbit's own KOReader
    plugin: its listing carries each library's book count, and opening one lists
    that library's books (the query carries the library id in its cache key).
    Also proves the root's section counts: the dashboard fetch is cached with
    the listings. The seed has a single library holding every book."""
    manifest, books = load_seed()

    with sd_card(manifest) as fs:  # no open book: boots to home
        script = ";".join([
            *TO_CATALOG,
            *presses(11000, "DOWN", 2), "11700:CONFIRM",  # root -> Libraries (3rd row)
            "14000:CONFIRM",                              # the seeded library -> its books
            "17000:QUIT",
        ])
        run_simulator(fs, input_script=script, choice="apply", timeout_s=60)

        caches = fs.list_caches()
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


def scenario_catalog_smart_scopes_browse() -> None:
    """SmartScopes is a browsable root section, like BookOrbit's own KOReader
    plugin: its listing names each scope, and opening one lists the books the
    scope resolves to (the query carries the scope id in its cache key). The
    seeded scope selects the Integration Shelf's members, so its books are the
    collection's -- reached through a saved server-side filter rather than
    through collection membership."""
    manifest, _ = load_seed()
    scope = manifest["smart_scope"]

    with sd_card(manifest) as fs:  # no open book: boots to home
        script = ";".join([
            *TO_CATALOG,
            *presses(11000, "DOWN", 4), "12400:CONFIRM",  # root -> SmartScopes (5th row)
            "14500:CONFIRM",                              # the seeded scope -> its books
            "17500:QUIT",
        ])
        run_simulator(fs, input_script=script, choice="apply", timeout_s=60)

        caches = fs.list_caches()
        roots = [c for c in caches if "sections" in c]
        assert roots and any(s["id"] == "smart-scopes" for s in roots[0]["sections"]), \
            f"SmartScopes missing from the cached root: {roots}"
        counts = [c for c in caches if "totalBooks" in c]
        assert counts and counts[0]["smartScopes"] == 1, \
            f"the root's SmartScopes count is wrong: {counts}"
        facets = [c for c in caches if "hasNext" in c]
        assert any(i["title"] == scope["name"] for c in facets for i in c["items"]), \
            f"the SmartScopes listing never loaded: {facets}"
        books = book_listings(caches)
        assert books, "the scope's books were never listed"
        listed = {b["title"] for b in books[0]["items"]}
        expected = {Path(b["file"]).stem for b in scope["books"]}
        assert listed == expected, f"SmartScope books mismatch: {listed} != {expected}"


def scenario_catalog_hides_non_epub() -> None:
    """The catalog lists only books it can download: the audiobook-only record,
    which shares an EPUB's title and author, is absent, while the book holding
    both an EPUB and an audio file stays. The seed uploads the audiobook after
    the library and marks it as being read, so an unfiltered listing would show
    it first in Recently added and among Continue reading. The root's two book
    counts must match those EPUB listings, not the dashboard's all-format
    totals, and Continue reading lists only the books being read. Expected
    numbers come from the server's own filtered listings."""
    manifest, _ = load_seed()
    non_epub = manifest["non_epub"]
    server = sim_device(manifest)
    epub_total = server.catalog_page(size=1, format="epub")["total"]
    reading_total = server.catalog_page(size=1, format="epub", readStatus="reading")["total"]
    # Guards on the fixture itself: without these gaps the test proves nothing.
    assert server.catalog_page(size=1)["total"] > epub_total, "the seed holds no non-EPUB book"
    assert server.catalog_page(size=1, readStatus="reading")["total"] > reading_total, \
        "the seeded audiobook is not marked as being read"
    assert reading_total <= 20, "Continue reading no longer fits on one page; check whole pages"

    with sd_card(manifest) as fs:  # no open book: boots to home
        script = ";".join([
            *TO_CATALOG,
            "11000:CONFIRM",                           # root -> Continue reading (1st row)
            "14000:BACK",                              # back to the root
            "16000:DOWN", "16350:CONFIRM",             # Recently added (2nd row)
            "19500:QUIT",
        ])
        run_simulator(fs, input_script=script, choice="apply", timeout_s=60)

        caches = fs.list_caches()
        counts = [c for c in caches if "totalBooks" in c]
        assert counts, "the root's section counts were never cached"
        assert counts[0]["totalBooks"] == epub_total, \
            f"All books count {counts[0]['totalBooks']} != {epub_total} EPUBs"
        assert counts[0]["inProgress"] == reading_total, \
            f"Continue reading count {counts[0]['inProgress']} != {reading_total} EPUBs being read"

        listings = listings_by_key(caches, 2, 4)  # (sort, readStatus)
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


def scenario_catalog_download_naming() -> None:
    """A catalog download lands where the account's KOReader file naming template
    says, folders created on the way, not under the "Title - Author.epub" this firmware
    used to hardcode. The server resolves the template per file and ships it as the
    file's devicePath, so the expected path is asked of the server rather than
    recomputed here: the test proves the firmware honours the answer. The download
    index records the book, read back in the layout docs/file-formats.md documents.

    Three runs on one card. The download restarts into a network boot of its own first,
    which ran the transfer and left no resume file behind. A second run proves that "On
    device" still counts the book, which it can only do from the download index. A
    third replaces the index with 135 other books, as if the file had been copied there:
    Confirm on the same row opens the copy without downloading it, and the index records
    it after the others."""
    manifest, _ = load_seed()
    sim = sim_device(manifest)

    with sd_card(manifest, download_folder="/Downloads") as fs:
        # A configured download folder: the template's folders hang off it, and
        # neither it nor they exist on this card yet.
        first_book_script = ";".join([
            *TO_ALL_BOOKS,
            "16000:CONFIRM",                              # first book -> download
            "24000:QUIT",                                 # only reached if no restart dropped it
        ])
        output = run_simulator(fs, input_script=first_book_script, choice="apply", timeout_s=90,
                               after_reboot_script="12000:QUIT")
        assert "Restarting into the download of book" in output, "the download did not restart first"
        resumed = output.index("Restarting into the download of book")
        assert f"Minimal network boot ready: target={BOOT_TARGET_BOOKORBIT_DOWNLOAD}" in output[resumed:], \
            "no download boot after the restart"
        assert "Downloading file" in output[resumed:], "the download boot did not run the transfer"
        assert not (fs.crosspoint / "bookorbit_download.json").exists(), "the download was left to resume"

        # The book that was downloaded is the first row of the listing the browser
        # cached, so the expectation follows the device's own view of the catalog
        # instead of assuming how the server ordered it.
        caches = fs.list_caches()
        listings = book_listings(caches)
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
        index = fs.read_download_index()
        assert index == [(first["id"], f"/Downloads/{device_path}", expected.stat().st_size)], \
            f"download index does not record the download: {index}"

        # Second run on the same card: the offline "On device" category must still
        # find the book now that the template filed it two folders deep. The catalog
        # root counts it on entry, which is where the scan reports what it found.
        output = run_simulator(fs, input_script=CATALOG_ROOT, choice="apply", timeout_s=60)
        assert "Local scan: kind=on-device count=1" in output, \
            "'On device' did not find the downloaded book inside the template's folders"

        # Third run: the same file, as another app or a copy over USB would leave it at the
        # template's path, unknown to the index (here a full one of other books), so its row
        # is not marked. Confirm finds it through the book's details and opens it rather than
        # downloading it again, and the index records it after the books it already held.
        book_path = f"/Downloads/{device_path}"
        others = [(-1 - i, f"/Downloads/Older/Book {i}.epub", 10) for i in range(135)]
        fs.write_download_index(others, version=2)
        output = run_simulator(fs, input_script=first_book_script, choice="apply", timeout_s=90,
                               after_reboot_script="6000:QUIT")
        assert "Restarting into the download of book" not in output, "the copy in place downloaded again"
        assert f"is already at {book_path}, opening it" in output, "Confirm did not find the copy in place"
        assert fs.read_download_index() == others + [
            (first["id"], book_path, expected.stat().st_size)], "the index did not record the copy after the others"
        state = json.loads((fs.crosspoint / "state.json").read_text())
        assert state.get("openEpubPath") == book_path, "Confirm did not open the copy in place"


def scenario_catalog_download_resumes() -> None:
    """A book download the connection drops resumes where it stopped instead of starting
    over, and only fails after three attempts in a row that bring nothing more; what it got
    stays on the card for the next download of that book. BookOrbit's KOReader route serves
    byte ranges only when the download names the delivery variant the book's details give,
    so every request must name it. A stand-in for the server breaks the connection.

    Four runs. On a first card, four attempts in a row are cut after 2000 bytes: each later
    one asks for the rest (Range) of the version it started (If-Range, the first answer's
    ETag) and gets it (206), four failed attempts where three used to end the download, and
    the fifth completes the book, byte for byte the server's file. On a second card, which
    holds another book's unfinished download: the first attempt is cut after 3000 bytes and
    the next three get no answer, so the download fails, with the 3000 bytes and their ETag
    kept and the other book's bytes gone. The next download resumes them and the reader is
    killed mid-transfer, as a reset or a flat battery would stop it. The last one resumes
    from where that left off and completes the book, leaving nothing staged behind."""
    manifest, _ = load_seed()
    sim = sim_device(manifest)
    script = ";".join([*TO_ALL_BOOKS, "16000:CONFIRM", "24000:QUIT"])
    staging = Path(".crosspoint") / "xfer" / "http-download"

    def staged(fs: SimFs) -> dict[str, int]:
        root = fs.fs / staging
        return {p.relative_to(root).as_posix(): p.stat().st_size for p in root.rglob("*") if p.is_file()}

    def books(fs: SimFs) -> list[Path]:
        return [p for p in fs.fs.rglob("*.epub") if staging not in p.relative_to(fs.fs).parents]

    def resumed(d: dict, offset: int, etag: str) -> bool:
        return d["range"] == f"bytes={offset}-" and d["if_range"] == etag and d["status"] == 206

    with server_cutting_downloads([2000] * 4) as (url, downloads), sd_card(manifest) as fs:
        fs.update_bookorbit(serverUrl=url)
        output = run_simulator(fs, input_script=script, choice="apply", timeout_s=120,
                               after_reboot_script="15000:QUIT")
        assert "Restarting into the download of book" in output, "the download did not start"
        first = book_listings(fs.list_caches())[0]["items"][0]
        epub = next(f for f in sim.catalog_book_detail(first["id"])["files"] if f["format"].lower() == "epub")
        variant = epub["downloadVariant"]
        assert len(downloads) == 5, f"expected 4 cut attempts and a complete one: {downloads}"
        assert all(d["path"].endswith(f"/download?variant={variant}") for d in downloads), \
            f"a download did not name its variant {variant!r}: {[d['path'] for d in downloads]}"
        whole = downloads[0]
        assert whole["range"] is None and whole["status"] == 200 and whole["etag"], \
            f"the first attempt should fetch the whole file under an ETag: {whole}"
        for attempt, d in enumerate(downloads[1:], start=1):
            assert resumed(d, 2000 * attempt, whole["etag"]), f"attempt {attempt + 1} did not resume: {d}"
        assert "Download attempt 4 failed" in output and "Download attempt 5 failed" not in output, \
            "the download did not keep going while it progressed"
        assert [p.read_bytes() for p in books(fs)] == [whole["body"]], "the resumed book differs from the server's file"
        assert not staged(fs) and not (fs.crosspoint / "bookorbit_partial.json").exists(), \
            "a completed download left its staged bytes or their record"

    with sd_card(manifest) as fs:
        record = fs.crosspoint / "bookorbit_partial.json"
        other = fs.fs / staging / "Other" / "Book.epub"
        other.parent.mkdir(parents=True)
        other.write_bytes(b"x" * 100)
        record.write_text(json.dumps({"fileId": -5, "path": "/Other/Book.epub", "variant": "original",
                                      "validator": '"another-book"'}))

        with server_cutting_downloads([3000, DROP, DROP, DROP]) as (url, downloads):
            fs.update_bookorbit(serverUrl=url)
            output = run_simulator(fs, input_script=script, choice="apply", timeout_s=120,
                                   after_reboot_script="15000:QUIT")
        whole = downloads[0]
        assert whole["status"] == 200 and whole["etag"] and len(downloads) == 4, \
            f"expected a cut answer, then three attempts without one: {downloads}"
        assert all(d["range"] == "bytes=3000-" and d["if_range"] == whole["etag"] for d in downloads[1:]), \
            f"the attempts after the cut did not ask for the rest: {downloads[1:]}"
        assert "3 in a row without progress)" in output, "the third attempt without progress did not end it"
        assert not books(fs), "the failed download left a book"
        assert list(staged(fs).values()) == [3000], f"the 3000 bytes received were not kept alone: {staged(fs)}"
        kept = json.loads(record.read_text())
        epub = next(f for f in sim.catalog_book_detail(
            book_listings(fs.list_caches())[0]["items"][0]["id"])["files"] if f["format"].lower() == "epub")
        assert (kept["fileId"], kept["variant"], kept["validator"]) == (
            epub["id"], epub["downloadVariant"], whole["etag"]), f"the kept bytes' record is wrong: {kept}"

        # Killed while the answer hangs: the reader never sees the transfer end.
        with server_cutting_downloads([stall(2000)]) as (url, downloads):
            fs.update_bookorbit(serverUrl=url)
            output = run_simulator(fs, input_script=script, choice="apply", timeout_s=45)
        assert "Resuming the 3000 bytes an earlier download of this book kept" in output, \
            "the kept bytes were not picked up"
        assert len(downloads) == 1 and resumed(downloads[0], 3000, whole["etag"]), \
            f"the download did not resume the kept bytes: {downloads}"
        assert list(staged(fs).values()) == [5000], f"the bytes received before the reset were lost: {staged(fs)}"

        with server_cutting_downloads([]) as (url, downloads):
            fs.update_bookorbit(serverUrl=url)
            run_simulator(fs, input_script=script, choice="apply", timeout_s=120, after_reboot_script="15000:QUIT")
        assert len(downloads) == 1 and resumed(downloads[0], 5000, whole["etag"]), \
            f"the download after the reset did not resume: {downloads}"
        assert [p.read_bytes() for p in books(fs)] == [whole["body"]], "the resumed book differs from the server's file"
        assert not staged(fs) and not record.exists(), "the completed download left its staged bytes or their record"


def scenario_catalog_book_actions() -> None:
    """A catalog book's action menu, opened by holding Confirm, and what Confirm does
    once the book is on the device. Four runs on one card, each on the first book of
    All books: the menu's single Download entry downloads it; the on-device menu's
    Re-download replaces it in place, keeping its reading progress; a plain Confirm
    then opens it instead of downloading it again; and the menu's Delete removes it
    along with its cache."""
    manifest, _ = load_seed()
    sim = sim_device(manifest)
    hold_first_book = "16000:CONFIRM:1500"  # past the 1 s threshold: opens the menu
    # The menu logs the FileBrowserAction it returns: without that, a hold that
    # simply downloaded on release (the old behaviour) would pass steps 1 and 2.
    action_download, action_redownload, action_delete = 65, 66, 0  # FileBrowserActionActivity.h
    # A download restarts into a boot of its own, which drops the first script: this one
    # ends that boot.
    after_download = "12000:QUIT"

    with sd_card(manifest, download_folder="/Downloads") as fs:
        # 1. Not on the device: the menu holds Download only, selected by default.
        script = ";".join(TO_ALL_BOOKS + [hold_first_book, "18500:CONFIRM", "26500:QUIT"])
        output = run_simulator(fs, input_script=script, choice="apply", timeout_s=90,
                               after_reboot_script=after_download)
        caches = fs.list_caches()
        listings = book_listings(caches)
        assert listings and listings[0]["items"], f"no book listing was cached: {caches}"
        first = listings[0]["items"][0]
        assert f"Book action {action_download} on book {first['id']}" in output, \
            "holding Confirm did not offer Download from the book's menu"
        epub = next(f for f in sim.catalog_book_detail(first["id"])["files"]
                    if f["format"].lower() == "epub")
        sim_path = f"/Downloads/{epub['devicePath']}"
        book = fs.path(sim_path)
        assert book.exists(), \
            f"the menu's Download did not fetch {first['title']!r} to {sim_path!r}"
        original = book.read_bytes()

        # 2. On the device: Re-download (second entry) replaces the file where it is.
        # Damage it without changing its size (the download index checks the size, so
        # the book stays recognised as on the device), and give it reading progress.
        damaged = bytearray(original)
        damaged[-64:] = bytes(64)
        book.write_bytes(bytes(damaged))
        write_progress_bin(fs.progress_file(sim_path), 3, 2, 10)
        script = ";".join(TO_ALL_BOOKS + [hold_first_book, "18500:DOWN", "18900:CONFIRM", "27000:QUIT"])
        output = run_simulator(fs, input_script=script, choice="apply", timeout_s=90,
                               after_reboot_script=after_download)
        assert f"Book action {action_redownload} on book {first['id']}" in output, \
            "the on-device menu's second entry is not Re-download"
        assert book.read_bytes() == original, "Re-download did not replace the damaged file"
        epubs = sorted(p.relative_to(fs.fs).as_posix() for p in fs.fs.rglob("*.epub*"))
        assert epubs == [sim_path.lstrip("/")], f"Re-download left other files behind: {epubs}"
        kept = read_progress_bin(fs.progress_file(sim_path))
        assert kept and (kept["spine"], kept["page"]) == (3, 2), \
            f"Re-download lost the book's reading progress: {kept}"

        # 3. A plain Confirm opens the on-device book. The catalog persists the path and
        # silent-restarts into the reader; synthetic input does not survive that
        # restart, so the run ends on the timeout.
        script = ";".join(TO_ALL_BOOKS + ["16000:CONFIRM"])
        output = run_simulator(fs, input_script=script, choice="apply", timeout_s=30)
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
        script = ";".join(TO_ALL_BOOKS + [
            hold_first_book, "18500:DOWN", "18850:DOWN", "19200:CONFIRM",
            "21000:DOWN", "21400:CONFIRM",
            "23500:QUIT",
        ])
        output = run_simulator(fs, input_script=script, choice="apply", timeout_s=60)
        assert f"Book action {action_delete} on book {first['id']}" in output, \
            "the on-device menu's third entry is not Delete"
        assert not book.exists(), "Delete left the book on the card"
        assert not fs.epub_cache_dir(sim_path).exists(), "Delete left the book's cache behind"


def scenario_catalog_deep_listing() -> None:
    """Issue #96: a long listing keeps only a window of pages in memory, its rows
    still numbered from the top of the listing. Walks All books down to its fourth
    page with Next presses, downloads the book selected there, then walks back up
    in the download boot. Proves that the rows in memory never exceed three pages,
    that the download fetched the book the selection was on, that the restored
    window holds that book's row (pages 3 and 4 only), and that walking back reloads
    an earlier page in front of the window from the list cache. Asserted through the
    firmware's per-page log line."""
    manifest, _ = load_seed()
    window_rows = 3 * 20  # BookOrbitCatalogWindow::MAX_PAGES + the page on its way in, of PAGE_SIZE rows

    with sd_card(manifest, download_folder="/Downloads") as fs:
        # 72 Next presses aim at row 72, on page 4. A press that lands while a page
        # loads can be missed, so the assertions follow the book the log names.
        script = ";".join([
            *TO_ALL_BOOKS,
            *presses(16000, "DOWN", 72, step_ms=400),
            "46000:CONFIRM",                              # the selected book -> download
            "60000:QUIT",                                 # only reached if no restart dropped it
        ])
        # The download boot rebuilds the listing around the book (pages 3 and 4, rows
        # 40-79), then 45 Previous presses walk back past the window's first row.
        back = presses(12000, "UP", 45, step_ms=400) + ["32000:QUIT"]
        output = run_simulator(fs, input_script=script, choice="apply", timeout_s=120,
                               after_reboot_script=";".join(back))

        marker = re.search(r"Restarting into the download of book (\d+)", output)
        assert marker, "the deep listing's download did not restart first"
        before, resumed = output[:marker.start()], output[marker.end():]

        loads = [(int(p), source, int(rows), int(start)) for p, source, rows, start in LISTING_PAGE_LOG.findall(output)]
        assert loads, "no listing page load was logged"
        assert max(rows for _, _, rows, _ in loads) <= window_rows, \
            f"more than {window_rows} rows were kept in memory: {loads}"
        browsed = [(p, start) for p, _, _, start in LISTING_PAGE_LOG.findall(before)]
        assert any(int(p) >= 4 for p, _ in browsed), f"browsing never reached page 4: {browsed}"
        assert any(int(start) > 0 for _, start in browsed), f"no page ever left the window: {browsed}"

        # The selected book, found by its row in the listing the device cached.
        pages = sorted((c for c in book_listings(fs.list_caches()) if c["total"] > 60), key=lambda c: c["page"])
        listing = [item for page in pages for item in page["items"]]
        book_id = int(marker.group(1))
        row = next((i for i, item in enumerate(listing) if item["id"] == book_id), None)
        assert row is not None and row >= 60, f"the downloaded book {book_id} is not on page 4 or later (row {row})"

        # The path as the firmware sanitized the server's devicePath (a seeded book can
        # have no author, which leaves an empty folder name the firmware drops).
        landed = re.search(r"Downloading file \d+ -> (\S.*?) \(free=", resumed)
        assert landed and fs.path(landed.group(1)).exists(), "the selected book was not downloaded"

        restored = [(int(p), int(rows), int(start)) for p, _, rows, start in LISTING_PAGE_LOG.findall(resumed)]
        last_restore = max(i for i, (p, _, _) in enumerate(restored) if p == row // 20 + 1)
        _, rows, start = restored[last_restore]
        assert start <= row < start + rows, \
            f"the restored window (rows {start}-{start + rows - 1}) does not hold row {row}"
        walked_back = restored[last_restore + 1:]
        assert any(start_row < start for _, _, start_row in walked_back), \
            f"walking back never put an earlier page in front of the window: {walked_back}"


def scenario_catalog_sort_filter() -> None:
    """All books is sorted by date added (Left), its direction flipped (Left, then the
    picker's first row) and filtered to the books being read (Right), the way the
    Library sorts on a button-only device. Each change refetches the listing with the
    server doing the work, so the rows must be the server's own answer to the same
    query. The first book of the result is then downloaded: the restart before the
    download must bring back that very listing from the list cache. The steps run in
    that order so that losing any part of the query on the way shows: a lost sort or
    direction misses the cache, a lost filter brings back a full page of All books.
    Sorted by date because the seeded books carry no title, author or series
    metadata, which leaves those orders to the book id both ways."""
    manifest, _ = load_seed()
    server = sim_device(manifest)
    newest = [i["id"] for i in server.catalog_page(format="epub", sort="recently_added")["items"]]
    oldest = [i["id"] for i in server.catalog_page(format="epub", sort="recently_added", order="asc")["items"]]
    reading = server.catalog_page(format="epub", sort="recently_added", order="asc", readStatus="reading")
    reading_ids = [i["id"] for i in reading["items"]]
    all_total = server.catalog_page(size=1, format="epub")["total"]
    # Guards on the fixture itself: an order or a filter that changes nothing proves nothing.
    assert newest != oldest, "flipping the date order changed nothing"
    assert 1 < reading["total"] < min(all_total, 20), \
        f"the reading books do not narrow All books to less than a page: {reading['total']}"

    with sd_card(manifest, download_folder="/Downloads") as fs:
        script = ";".join([
            *TO_ALL_BOOKS,
            "16500:LEFT",                                 # sort picker, on Title
            "17000:UP", "17400:CONFIRM",                  # -> Date Added, newest first
            "20500:LEFT",                                 # sort picker, on Date Added
            "21000:UP", "21400:UP", "21800:CONFIRM",      # past List View -> the direction row
            "24500:RIGHT",                                # filter picker, on All
            "25000:DOWN", "25400:DOWN", "25800:CONFIRM",  # -> Reading
            "29000:CONFIRM",                              # the first book -> download
            "45000:QUIT",                                 # only reached if no restart dropped it
        ])
        output = run_simulator(fs, input_script=script, choice="apply", timeout_s=90,
                               after_reboot_script="15000:QUIT")

        listings = listings_by_key(fs.list_caches(), 2, 5)  # (sort, readStatus, order)
        expected = {
            ("recently_added", "", ""): newest,
            ("recently_added", "", "asc"): oldest,
            ("recently_added", "reading", "asc"): reading_ids,
        }
        for key, ids in expected.items():
            assert key in listings, f"never listed sort={key[0]} readStatus={key[1]!r} order={key[2]!r}: {sorted(listings)}"
            assert [i["id"] for i in listings[key]["items"]] == ids, f"the {key} listing is not the server's"
        assert listings[("recently_added", "reading", "asc")]["total"] == reading["total"], \
            "the filtered listing's total is not the server's"

        marker = re.search(r"Restarting into the download of book (\d+)", output)
        assert marker, "the sorted listing's download did not restart first"
        assert int(marker.group(1)) == reading_ids[0], \
            f"downloaded book {marker.group(1)}, not the first of the filtered listing ({reading_ids[0]})"
        resumed = output[marker.end():]
        assert re.search(r"Downloading file \d+ -> ", resumed), "the download boot never downloaded the book"
        # The listing the download boot rebuilds: one page, as many rows as the filter
        # keeps, from the cache.
        restored = [(source, int(rows)) for _, source, rows, _ in LISTING_PAGE_LOG.findall(resumed)]
        assert restored and restored[-1] == ("cache", len(reading_ids)), \
            f"the download boot rebuilt another listing: {restored} (expected {len(reading_ids)} rows from cache)"


WANT_TO_READ_BOOKS = (60, 61)  # library indices outside every seeded range


def scenario_catalog_sections_and_shelf() -> None:
    """The data behind the catalog's list sections and its Want to Read shelf. All
    books (sorted by title) keeps, with its first page, where each letter starts, as
    the server ranks it; Recently added keeps each book's date added for its day
    headings. Two books marked want to read put a Want to Read row after the server's
    sections, which lists the server's own shelf. The headings themselves are drawn
    from this data and are checked on screen, not here."""
    manifest, books = load_seed()
    server = sim_device(manifest)
    # Searching finds nothing by a filename stem, which is all the seeded books are titled.
    listed = [item for page in (1, 2)
              for item in server.catalog_page(page=page, size=100, format="epub", sort="recently_added")["items"]]
    by_title = {item["title"]: item["id"] for item in listed}
    marked = []
    for index in WANT_TO_READ_BOOKS:
        title = Path(books[index]["file"]).stem
        assert title in by_title, f"seeded book {title!r} not found on the server"
        server.set_read_status(by_title[title], "want_to_read")
        marked.append(by_title[title])
    letters = server.catalog_page(format="epub", sort="title", buckets="true", size=1)["buckets"]
    shelf_ids = want_to_read_epubs(server)
    assert set(marked) <= set(shelf_ids), f"the marked books are not on the shelf: {shelf_ids}"

    with sd_card(manifest) as fs:  # no open book: boots to home
        script = ";".join([
            *TO_CATALOG,
            "13000:DOWN", "13400:CONFIRM",                # Recently added (2nd row)
            "16500:BACK",
            *presses(18500, "DOWN", 7), "20950:CONFIRM",  # root -> All books (8th row)
            "24000:BACK",
            *presses(26000, "DOWN", 8), "28800:CONFIRM",  # root -> Want to Read (9th row)
            "32000:QUIT",
        ])
        run_simulator(fs, input_script=script, choice="apply", timeout_s=60)

        caches = fs.list_caches()
        counts = [c for c in caches if "totalBooks" in c]
        assert counts and counts[0]["wantToRead"] == len(shelf_ids), \
            f"Want to Read count {counts and counts[0].get('wantToRead')} != {len(shelf_ids)} shelved EPUBs"
        listings = listings_by_key(caches, 1, 6)  # (page, sort, readStatus, order, shelf)
        by_title = listings.get(("1", "title", "", "", ""))
        assert by_title, f"All books was never listed: {sorted(listings)}"
        assert [[b["key"], b["index"]] for b in letters] == by_title.get("letters"), \
            f"letters {by_title.get('letters')} != the server's {letters}"
        recent = listings.get(("1", "recently_added", "", "", ""))
        assert recent and all(i.get("addedAt", 0) > 0 for i in recent["items"]), \
            "Recently added rows carry no date added"
        listed = listings.get(("1", "", "", "", "want-to-read"))
        assert listed, f"the Want to Read shelf was never listed: {sorted(listings)}"
        assert [i["id"] for i in listed["items"]] == shelf_ids, \
            f"shelf {[i['id'] for i in listed['items']]} != the server's {shelf_ids}"
