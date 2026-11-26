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
- **Harness**: [harness/run_scenarios.py](harness/run_scenarios.py) runs the
  scenarios of [harness/scenarios/](harness/scenarios/), one module per
  functional area (see [Scenarios by area](#scenarios-by-area)). Each one
  prepares an isolated `fs_/` SD directory ([harness/sim.py](harness/sim.py):
  settings, BookOrbit credentials, seeded books), launches the simulator binary
  with `SDL_VIDEODRIVER=dummy`, drives it, and asserts on both sides: the SD tree,
  read through [harness/formats.py](harness/formats.py) (progress files, clipping
  and bookmark stores, download index), and the server state via API
  ([harness/server.py](harness/server.py)). The simulator's silent
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

# one scenario, verbose; one area; every scenario by area, with what it proves
python3 test/integration/harness/run_scenarios.py --scenario sync_progress_pull -v
python3 test/integration/harness/run_scenarios.py --area catalog
python3 test/integration/harness/run_scenarios.py --list
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

## Scenarios by area

Each scenario's docstring says what it proves; `run_scenarios.py --list` prints
them by area.

| Area | Module | What it covers |
| --- | --- | --- |
| `progress` | [progress.py](harness/scenarios/progress.py) | Reading position through the Smart sync and Sync on Sleep: pull, push, a book opened in the web reader (#91), a conflict, a progress reset, a sleep sync cancelled by a press. |
| `highlights` | [highlights.py](harness/scenarios/highlights.py) | Highlight exchange both ways and in batches, deletions carried by the complete key set and the guard when it is lost, copies the server offers back, notes, positions minted outside `<p>` (#84). |
| `bookmarks` | [bookmarks.py](harness/scenarios/bookmarks.py) | Bookmark exchange both ways, deletions, several bookmarks in one chapter (#120) and the recovery of those earlier builds lost. |
| `home` | [home.py](harness/scenarios/home.py) | The BookOrbit Home theme: the tile's sync and its snapshot, the Library shelves and their sources, a download from a shelf, the sleep sync's refresh. |
| `catalog` | [catalog.py](harness/scenarios/catalog.py) | Catalog browsing, EPUB-only listings, sort and filter, list sections and the Want to Read shelf, the page window of long listings (#96), downloads and the book action menu. |
| `local_books` | [local_books.py](harness/scenarios/local_books.py) | Books on the card: the download index (BOD1 migration, past BOD1's cap), "On device" on a 210-book card, the card scan from the sleep sync and the Home tile, moves through the file transfer portal. |
| `book_update` | [book_update.py](harness/scenarios/book_update.py) | A copy BookOrbit has rewritten since: its changes held back, the update offer, highlights carried to the new copy. |

A new scenario is a `scenario_<name>` function in its area's module. A book with
no seeded server state takes a library index of its own in `FRESH_BOOK`
([harness/server.py](harness/server.py)), so runs never share one.

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
