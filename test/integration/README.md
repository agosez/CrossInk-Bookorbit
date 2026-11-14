# BookOrbit Integration Tests

End-to-end tests that run the simulator firmware against a real BookOrbit
server, exercising the full sync logic: progress push/pull, highlight and
bookmark exchange, deletions, catalog downloads. Runnable locally before a
push and on CI.

## Architecture

```
┌────────────────────────────┐        ┌─────────────────────────────┐
│ docker compose             │  HTTP  │ Simulator process(es)       │
│  ├─ bookorbit (ghcr image) │◄───────┤  fs_/ dir = SD card         │
│  │   BOOK_DOCK_PATH=/dock  │        │  bookorbit.json → server    │
│  └─ postgres (pgvector)    │        │  scenario driven by env     │
└────────────▲───────────────┘        └──────────────▲──────────────┘
             │ seeds                                  │ runs & asserts
        seed/seed.py  ◄──────── harness/run_scenarios.py
```

- **Server**: the official BookOrbit image plus Postgres, via
  [docker-compose.yml](docker-compose.yml). Test-only credentials live in
  [.env](.env); the app listens on `127.0.0.1:3999`.
- **Library**: [seed/make_library.py](seed/make_library.py) generates ~100
  EPUB variants from `test/epubs/` fixtures (unique title/author/uuid, and a
  verified-unique KOReader partial-MD5 each, since BookOrbit identifies books
  by content) into `library/`, the local reference copy. Every book except
  each 10th embeds a generated PNG cover, which the server extracts on
  ingestion: the BookOrbit Home scenarios decode those thumbnails, and the
  coverless books keep the placeholder path covered. The seeder copies
  them into the book-dock volume — the dock consumes its files on finalize.
- **Seed**: [seed/seed.py](seed/seed.py) bootstraps the server (setup token →
  user → KOReader sync credentials), waits for the library ingestion, then
  plays a **synthetic device** over the same kosync API the firmware speaks
  (`/api/v1/koreader/...`) to pre-load reading progress, highlights and
  bookmarks on a subset of books. It also uploads the non-EPUB records the
  catalog must hide or keep: an audiobook-only book (a generated silent MP3)
  that shares an EPUB's title and author, and an audio file attached to
  another EPUB. Both are marked as being read. Two-device scenarios use this synthetic
  device as the "other" reader — the simulator's MAC is fixed, so two real
  simulator instances would share one device id.
- **Harness**: [harness/run_scenarios.py](harness/run_scenarios.py) prepares an
  isolated `fs_/` SD directory per scenario (settings, BookOrbit credentials,
  seeded books), launches the simulator binary with `SDL_VIDEODRIVER=dummy`,
  drives it, and asserts on both sides: the SD tree (progress files, clipping
  and bookmark stores) and the server state via API. The simulator's silent
  network reboot re-execs the process in place, so a sync flow runs unattended
  end to end. Synthetic input does not survive that reboot (the simulator only
  promotes `_AFTER_WAKE` scripts on deep-sleep wakes), so the sync's screens
  are answered by firmware-side scenario hooks in `BookOrbitSyncActivity`'s
  `#ifdef SIMULATOR` blocks: `CROSSINK_SIM_BOOKORBIT_CHOICE=apply|upload`
  answers the choice screens, `CROSSINK_SIM_BOOKORBIT_QUIT_AFTER_SYNC=1` ends
  the process with a `sync scenario finished (state=N)` marker once the sync
  settles.

The simulator binary is a plain host process: locally it runs natively
(fast pre-push loop); on CI the same harness runs inside a Linux container.

## Running locally

Requires a Docker-compatible runtime (Docker Desktop, OrbStack or colima) and
the `simulator` PlatformIO environment building.

```sh
# everything: server up + seed + build + scenarios (also works as a pre-push hook:
#   ln -s ../../test/integration/run_local.sh .git/hooks/pre-push )
PIO=~/.platformio/penv/bin/pio test/integration/run_local.sh

# or step by step:
python3 test/integration/seed/make_library.py          # one-time / fixture changes
mkdir -p test/integration/library-dock                 # Linux: before Docker makes it root's
chmod a+rwx test/integration/library-dock
docker compose -f test/integration/docker-compose.yml up -d --wait
python3 test/integration/seed/seed.py                  # idempotent
pio run -e simulator
python3 test/integration/harness/run_scenarios.py

# one scenario, verbose
python3 test/integration/harness/run_scenarios.py --scenario sync_progress_pull -v
```

`docker compose ... down -v` resets the server completely; `seed.py` starts
from a database it recognizes as already seeded without duplicating anything.

### A real device against this server

[device_kit.py](device_kit.py) builds an SD-card kit for one heavy sync: a
generated 20-chapter book with 100 local highlights and 600 reading events to
upload, and 60 highlights from another device on the server to pull. With
`--start-caddy` it puts Caddy (internal TLS, the Go TLS stack behind #96) in
front of the server on this Mac's LAN address. `--mac` also writes the
`bookorbit.json` for that device; without it, enter the printed server, user
and password on the device. Once a device has synced a book, take the next
`--variant`.

The simulator reaches Caddy only with its CA, since its HTTP goes through
`curl`, which verifies certificates where the firmware does not:
`docker cp crossink-kit-caddy:/data/caddy/pki/authorities/local/root.crt .`
then `CURL_CA_BUNDLE=root.crt`.

## Continuous integration

The `integration` job in [ci.yml](../../.github/workflows/ci.yml) runs the same
chain on ubuntu: SDL2, OpenSSL + PlatformIO (two Linux-only link flags go in
through `PLATFORMIO_BUILD_FLAGS`), the simulator built after applying
[platformio.local.example.ini](../../platformio.local.example.ini) (cold-build
include fix) and [ci/patch_simulator_package.py](ci/patch_simulator_package.py)
(HAL shims the fetched simulator package does not carry yet — the patcher is
idempotent and becomes a no-op as upstream catches up), then compose up, seed,
scenarios, compose down.

It is the only Linux build of the simulator, and the package is fetched
unpinned, so it is also where an upstream simulator change shows first: a
patcher step that reports `anchor not found` means upstream rewrote the code
that step edits. Check whether it now carries the fix in its own words, and add
that spelling to the step's markers.

## Scenario inventory

| Scenario | What it proves |
| --- | --- |
| `sync_progress_pull` | A further server position (seeded by the synthetic device) is applied to both local progress homes. |
| `sync_progress_push` | A further local position reaches the server under the simulator's device id. |
| `sleep_sync_push` | With Sync on Sleep on, putting the device to sleep hands over to the sleep sync, which uploads a further local position from behind the sleep screen. Its sync marker is the 12-byte one earlier releases wrote, so the scenario also proves those still read. |
| `sleep_sync_after_web_open` | Issue #91, two runs on one card: a manual sync uploads the device's position, the book is then opened in BookOrbit's web reader (a save through the web API, which the server dates as of now and serves as `bookorbit-web`), and the device reads on and goes to sleep. The server's date moved but its position did not, so the sleep sync uploads instead of leaving the progress for a manual sync. The web save is checked to be the one the server serves, dated after the device's sync; without that the scenario would pass on dates alone. |
| `sleep_sync_conflict` | The guard on the scenario above: the peer device read past the position this device last synced, and this device read on too. The sleep sync leaves both positions and its own history untouched. |
| `sleep_sync_keeps_progress_reset` | The other guard: the book's progress is reset in BookOrbit, which the server serves as the start of the book dated now, under the same `bookorbit-web` id as a web reader save. The device is further on, but the sleep sync does not upload over the reset, which the server would accept and then ignore; the reset is still served afterwards. |
| `sleep_sync_cancel` | A Power press during the sleep sync cancels it and boots the device the way a power-button wake does. |
| `highlight_pull` | A highlight created by the peer device lands in the local clipping store, with a minted position record. |
| `highlight_push` | A pre-seeded local highlight (clipping + position record, what the reader persists) reaches the server and is offered to a device that never saw it. |
| `highlight_push_backlog` | Twenty local highlights, listed out of datetime order, reach the server in one sync (batches of 8 on one session), and the watermark ends on the newest. |
| `highlight_pull_backlog` | Twenty highlights from the peer reach the device in one sync: each acknowledged batch of 8 makes room for the next pull. |
| `highlight_delete_propagates_large` | On a book of 80 highlights, past the 64 a key set used to be held to, deleting two on the device deletes them on the server, and the summary counts them. |
| `highlight_mint_outside_paragraph` | Highlights saved without a position (what the reader left before the #84 fix) in a heading, a list item and a control `<p>` are positioned by the reader itself, one per wake into their chapter, and reach the server at their exact text node. The heading precedes every `<p>`, so its paragraph hint is 0: the old resolver rejected it, and retrying it on every visit starved the other two. The `<p>` keeps the position the `<p>`-only resolver gave it. |
| `highlight_delete_propagates` | Two runs on one SD: pull a highlight, delete its clipping locally, sync again — the complete key set deletes it server-side. |
| `highlight_delete_guard` | Same, but with the sync-state store lost between runs: the guard reports no key set and the server keeps the highlight. |
| `bookmark_pull` | A bookmark created by the peer lands in the local bookmark store, with a minted position record. |
| `bookmark_push` | A pre-seeded local bookmark reaches the server and is offered to a device that never saw it. |
| `catalog_collections_browse` | Scripted UI navigation: home menu → BookOrbit catalog → Collections → the seeded collection; asserted through the browser's list cache. Browsing never silent-reboots before exit, so input scripts drive it end to end; only a download restarts (see below). |
| `catalog_empty_listing_back` | Opening the seeded empty collection shows the no-entries error; Back climbs out of it onto the row it left, and the next listing still loads (regression: Back reloaded the same empty listing forever). |
| `catalog_libraries_browse` | Libraries as a browsable root section: its listing carries the seeded library's book count, opening it lists that library's books (cache key carries the library id), and the root's per-section counts (dashboard fetch) are cached. |
| `catalog_hides_non_epub` | Continue reading and Recently added list only EPUBs: the audiobook-only record is absent, the EPUB+audio record stays, and the root's two book counts equal those listings' totals rather than the dashboard's all-format ones. Continue reading lists only books being read. Expected numbers come from the server's own filtered listings. |
| `catalog_download_naming` | A download lands where the account's KOReader file naming template says, folders created on the way, instead of a flat `Title - Author.epub`. The expected path is the `devicePath` the server itself resolved for the book the browser listed first, so the test never reimplements the template. The simulator's X4 profile has no PSRAM, so the download restarts into a network boot of its own first, like an X3/X4 does (issue #96): the run checks that hand-over, that the transfer ran in the new boot and that it left no resume file behind. A second run then proves "On device" still counts the book, which it can only do from the download index. |
| `catalog_book_actions` | Four runs on one card around the first book of All books. Holding Confirm opens a menu whose only entry, Download, fetches the book. Once the book is on the device, Re-download replaces it where it is and keeps its reading progress; the book is damaged first, at the same size, so the check can tell the copies apart. A plain Confirm then opens the book in the reader rather than downloading it again, and Delete removes it along with its cache. Each menu choice is checked against the action the firmware logs, since a hold that simply downloaded on release would otherwise pass. |
| `catalog_deep_listing` | Issue #96: All books (five pages) walked to page 4 with Next presses, the selected book downloaded, then 45 Previous presses in the download boot. The rows in memory never exceed three pages, the download fetches the book the selection was on, the restored window holds its row (pages 3 and 4 only), and walking back puts page 2 in front of the window from the list cache. Asserted through the firmware's per-page log line. |
| `catalog_sort_filter` | All books sorted by date added (Left), its direction flipped (the picker's first row) and filtered to the books being read (Right), then the first book downloaded. Each listing matches the server's answer to the same `sort`/`order`/`readStatus` query, the download is that listing's first book, and the download boot rebuilds that exact listing from the list cache. Sorted by date because the seeded books carry no title, author or series metadata. |
| `catalog_sections_and_shelf` | The data behind the list sections and the Want to Read shelf. Two books are marked want to read first. All books keeps, with its first page, where each letter starts as the server ranks it; Recently added keeps each book's date added; the root's Want to Read row counts the server's shelf and lists exactly it. The headings drawn from that data are checked on screen. |

### Repeatability

The simulator's device id is fixed (MAC-derived), so its server-side sync
state persists across runs. Highlight scenarios stay repeatable by creating a
genuinely new annotation each run (unique text). Bookmarks dedupe server-side
on their converted location (paragraph precision), so a "new" bookmark lands
on the same server row: `bookmark_push` therefore keeps a FIXED identity
(datetime+pos) across runs — re-minting the identity of the same location is
what a real device never does, and it tombstones the row through the
complete-key-set diff — while `bookmark_pull` first detaches the simulator's
links (impersonated empty-complete exchange + deletion acks) so the re-minted
peer bookmark is offered again.

Planned next: catalog download into the configured folder plus on-device
marker, moved-book content-hash continuity, bookmark deletion propagation and
its lost-store guard.

## Notes

- The firmware side is driven through seeded settings (power shortcut =
  BookOrbit Sync, Smart sync behavior, `state.json` naming the open book) plus
  one `CROSSPOINT_SIM_INPUT_SCRIPT` power press; everything after the network
  reboot is driven by the firmware-side hooks above.
- Fixture books have 5 spine items; positions seeded beyond the spine range
  are clamped back to the beginning by the sync's own guard.
- Endpoints and payloads mirror `lib/BookOrbitSync/BookOrbitSyncClient.cpp`;
  when the protocol evolves there, evolve `seed/kosync.py` with it.
