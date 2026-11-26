"""The BookOrbit Home theme: the tile's sync and the snapshot the page draws from, the
Library shelves and the sources they list, a download started from a shelf, and the
sleep sync's refresh of the snapshot."""

from __future__ import annotations

import json
import struct

from formats import listings_by_key
from server import load_seed, server_with_failing_progress, sim_device, want_to_read_epubs
from sim import BOOKORBIT_SYNC_FAILED, HOME_TILE_SYNC, UI_THEME_BOOKORBIT, SimFs, run_simulator, sd_card, sleep_sync

# /.crosspoint/bookorbit_home.bin, BookOrbitHomeSnapshot.
# BookOrbitHomeBook: int32 id, uint32 coverVersion, uint8 progress, bool hasCover, char[46].
HOME_BOOK = struct.Struct("<iIBB46s")
# BookOrbitHomeShelf: its BookOrbitHomeShelfSource (kind, char id[20], char name[46]), the
# book count, then its four books.
HOME_SHELF_HEAD = struct.Struct("<B20s46sB")
HOME_SHELF_SIZE = HOME_SHELF_HEAD.size + 4 * HOME_BOOK.size
HOME_SHELF_KIND = {"discover": 0, "recently_added": 1, "want_to_read": 2, "collection": 3, "smart_scope": 4}


def home_snapshot_header(fs: SimFs) -> tuple[int, int] | None:
    """The Home snapshot's (version, stored struct size), or None without a snapshot."""
    path = fs.crosspoint / "bookorbit_home.bin"
    if not path.exists():
        return None
    magic, version, size = struct.unpack_from("<IHH", path.read_bytes())
    assert magic == 0x484F4242, f"unexpected Home snapshot magic {magic:#x}"
    return version, size


def home_snapshot_continue_total(fs: SimFs) -> int:
    """The snapshot's continueTotal: the eighth 4-byte field of BookOrbitHomeSnapshot."""
    return struct.unpack_from("<i", (fs.crosspoint / "bookorbit_home.bin").read_bytes(), 8 + 7 * 4)[0]


def home_snapshot_shelves(fs: SimFs) -> list[dict]:
    """The snapshot's two Library shelves, the last fields of BookOrbitHomeSnapshot."""
    raw = (fs.crosspoint / "bookorbit_home.bin").read_bytes()
    _, version, size = struct.unpack_from("<IHH", raw)
    assert version == 3, f"unexpected Home snapshot version {version}"
    shelves = []
    for index in range(2):
        base = 8 + size - (2 - index) * HOME_SHELF_SIZE
        kind, source_id, name, count = HOME_SHELF_HEAD.unpack_from(raw, base)
        ids = [HOME_BOOK.unpack_from(raw, base + HOME_SHELF_HEAD.size + i * HOME_BOOK.size)[0]
               for i in range(count)]
        shelves.append({"kind": kind, "sourceId": source_id.split(b"\0")[0].decode(),
                        "name": name.split(b"\0")[0].decode(), "ids": ids})
    return shelves


def scenario_home_theme_refresh() -> None:
    """The BookOrbit Home's tile refreshes its snapshot, from a boot with no open book,
    and decodes every cover it fetched."""
    manifest, _ = load_seed()
    with sd_card(manifest) as fs:
        fs.write_settings(uiTheme=UI_THEME_BOOKORBIT)
        assert home_snapshot_header(fs) is None
        run_simulator(fs, input_script=HOME_TILE_SYNC, choice="none", timeout_s=14)
        assert home_snapshot_header(fs) is not None, "the tile wrote no Home snapshot"
        covers = fs.crosspoint / "bookorbit_home"
        assert list(covers.glob("*.bmp")), "the refresh decoded no cover"
        assert not list(covers.glob("*.jpg")), "a cover stayed pending with Wi-Fi off"


def scenario_home_download_returns_home() -> None:
    """A book downloaded from a BookOrbit Home shelf ends on Home, not on a catalog listing."""
    manifest, _ = load_seed()
    with sd_card(manifest) as fs:
        fs.write_settings(uiTheme=UI_THEME_BOOKORBIT)
        run_simulator(fs, input_script=HOME_TILE_SYNC, choice="none", timeout_s=14)
        assert home_snapshot_header(fs) is not None, "the tile wrote no Home snapshot"

        # Library page, then its first stop: the first Discover cover, not on the card yet.
        output = run_simulator(fs, input_script="3000:RIGHT:120;4500:CONFIRM:120", choice="none", timeout_s=60)
        assert "Restarting into the download of book" in output, "the shelf book did not start a download"
        downloaded = output.index("Downloading file")
        after = output[downloaded:]
        assert "Card space read" in after, "Home did not open after the download"
        assert "Listing page" not in after, "the download went back to a catalog listing"
        assert not (fs.crosspoint / "bookorbit_download.json").exists(), "the download was left to resume"


def scenario_home_also_reading_link() -> None:
    """The BookOrbit Home's "All (N) >" link opens the catalog on Continue reading, over
    the root that Back returns to, and N is that listing's total as the sync counted it."""
    manifest, _ = load_seed()
    reading_total = sim_device(manifest).catalog_page(size=1, format="epub", readStatus="reading")["total"]
    with sd_card(manifest) as fs:
        fs.write_settings(uiTheme=UI_THEME_BOOKORBIT)
        run_simulator(fs, input_script=HOME_TILE_SYNC, choice="none", timeout_s=14)
        assert home_snapshot_header(fs) is not None, "the tile wrote no Home snapshot"
        assert home_snapshot_continue_total(fs) == reading_total, \
            f"snapshot counts {home_snapshot_continue_total(fs)} books in progress, the server {reading_total}"

        # Without books the link is the page's third stop, after the BookOrbit tile.
        script = ";".join(["3000:DOWN:120", "3500:DOWN:120", "4500:CONFIRM:120", "14000:QUIT"])
        run_simulator(fs, input_script=script, choice="none", timeout_s=40)

        caches = fs.list_caches()
        assert any("sections" in c for c in caches), "the catalog root was never loaded"
        listings = set(listings_by_key(caches, 2, 4))  # (sort, readStatus)
        assert listings == {("recently_read", "reading")}, f"the link did not open Continue reading: {listings}"


def scenario_home_shelf_sources() -> None:
    """The BookOrbit Home's two Library shelves list what BookOrbit Sync > Home chose: a
    collection and the want-to-read shelf from bookorbit.json first, then the top shelf
    switched to the SmartScope through the screens. Refresh lists restarts into its own
    network boot and saves the server's collections and SmartScopes; leaving the screen
    with a changed shelf syncs the Home, whose snapshot then lists the scope's books."""
    manifest, _ = load_seed()
    server = sim_device(manifest)
    collection, scope = manifest["collection"], manifest["smart_scope"]

    def listing_ids(**params) -> set[int]:
        return {b["id"] for b in server.catalog_page(size=50, format="epub", **params)["items"]}

    collection_ids = listing_ids(collectionId=collection["id"])
    scope_ids = listing_ids(smartScopeId=scope["id"])
    want_ids = set(want_to_read_epubs(server))
    assert collection_ids and scope_ids and want_ids, "the seed has no books to shelve"

    with sd_card(manifest) as fs:  # no open book: Power opens BookOrbit Sync
        fs.update_settings(uiTheme=UI_THEME_BOOKORBIT)
        # Each shelf is "kind:id:name"; only a collection or SmartScope has an id and name.
        fs.update_bookorbit(homeShelfTop=f"{HOME_SHELF_KIND['collection']}:{collection['id']}:{collection['name']}",
                            homeShelfBottom=f"{HOME_SHELF_KIND['want_to_read']}::")

        run_simulator(fs, input_script=HOME_TILE_SYNC, choice="none", timeout_s=14)
        top, bottom = home_snapshot_shelves(fs)
        assert top["kind"] == HOME_SHELF_KIND["collection"] and top["sourceId"] == str(collection["id"]), \
            f"the top shelf does not list the collection: {top}"
        assert top["name"] == collection["name"], f"the collection shelf lost its name: {top}"
        assert set(top["ids"]) <= collection_ids and len(top["ids"]) == min(4, len(collection_ids)), \
            f"collection shelf {top['ids']} is not the collection's {collection_ids}"
        assert bottom["kind"] == HOME_SHELF_KIND["want_to_read"], f"the bottom shelf is not Want to read: {bottom}"
        assert set(bottom["ids"]) <= want_ids and len(bottom["ids"]) == min(4, len(want_ids)), \
            f"want-to-read shelf {bottom['ids']} is not the server's {want_ids}"

        # Power (BookOrbit Sync without a book) opens its settings; Up wraps to Home, the
        # last row, and Up again to Refresh lists, which restarts into the shelves' boot.
        script = "3000:POWER:120;4500:UP:120;5500:CONFIRM:120;6500:UP:120;7500:CONFIRM:120"
        # That boot fetches, then shows the rows on Refresh: Up twice to the top shelf, whose
        # popup lists Discover, Recently added and Want to Read, then the two collections
        # and the scope, on the collection it shows. Down twice to the scope, then Back.
        after = ";".join(["9000:UP:120", "9500:UP:120", "10500:CONFIRM:120", "11500:DOWN:120",
                          "12000:DOWN:120", "13000:CONFIRM:120", "14500:BACK:120"])
        output = run_simulator(fs, input_script=script, choice="none", timeout_s=40, after_reboot_script=after)
        assert "Saved 3 shelf options" in output, "Refresh lists did not save both collections and the scope"
        assert "Home shelves changed: syncing the Home" in output, "leaving the screen did not sync the Home"

        saved = json.loads((fs.crosspoint / "bookorbit.json").read_text())
        assert saved["homeShelfTop"] == f"{HOME_SHELF_KIND['smart_scope']}:{scope['id']}:{scope['name']}", \
            f"the top shelf setting is not the scope: {saved['homeShelfTop']}"
        assert saved["homeShelfBottom"] == f"{HOME_SHELF_KIND['want_to_read']}::", \
            f"the bottom shelf setting moved: {saved['homeShelfBottom']}"
        top, bottom = home_snapshot_shelves(fs)
        assert top["kind"] == HOME_SHELF_KIND["smart_scope"] and top["sourceId"] == str(scope["id"]), \
            f"the Home sync did not follow the new top shelf: {top}"
        assert set(top["ids"]) <= scope_ids and len(top["ids"]) == min(4, len(scope_ids)), \
            f"scope shelf {top['ids']} is not the scope's {scope_ids}"
        assert bottom["kind"] == HOME_SHELF_KIND["want_to_read"], f"the bottom shelf changed: {bottom}"


def scenario_sleep_sync_refreshes_home() -> None:
    """Sync on Sleep also refreshes the Home snapshot, but only under the BookOrbit theme."""
    manifest, _ = load_seed()
    for theme, expected in ((UI_THEME_BOOKORBIT, True), (None, False)):
        with sd_card(manifest) as fs:
            fs.enable_sync_on_sleep()
            if theme is not None:
                fs.update_settings(uiTheme=theme)
            fs.open_book(manifest["progress"][2], progress=(4, 0, 1))
            sleep_sync(fs)
            refreshed = home_snapshot_header(fs) is not None
            assert refreshed == expected, f"theme {theme}: Home snapshot written={refreshed}, expected={expected}"


def scenario_sleep_sync_failure_keeps_home() -> None:
    """A sleep sync that fails for the book leaves the Home snapshot alone: the tile dates
    the snapshot as the last sync, so refreshing it would report the failed sync as done.
    The server's progress endpoint fails while its dashboard still answers."""
    manifest, _ = load_seed()

    with sd_card(manifest) as fs, server_with_failing_progress() as url:
        fs.enable_sync_on_sleep()
        fs.update_bookorbit(serverUrl=url)
        fs.update_settings(uiTheme=UI_THEME_BOOKORBIT)
        fs.open_book(manifest["progress"][2], progress=(4, 0, 1))

        output = sleep_sync(fs)
        assert "Restarting into the BookOrbit sleep sync" in output, "sleep did not hand over to the sleep sync"
        assert "Sleep sync over (state=%d)" % BOOKORBIT_SYNC_FAILED in output, "the sleep sync did not fail"
        assert home_snapshot_header(fs) is None, "a failed sleep sync wrote a Home snapshot"
