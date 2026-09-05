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
  by content) into `library/`, the local reference copy. The seeder copies
  them into the book-dock volume — the dock consumes its files on finalize.
- **Seed**: [seed/seed.py](seed/seed.py) bootstraps the server (setup token →
  user → KOReader sync credentials), waits for the library ingestion, then
  plays a **synthetic device** over the same kosync API the firmware speaks
  (`/api/v1/koreader/...`) to pre-load reading progress, highlights and
  bookmarks on a subset of books. Two-device scenarios use this synthetic
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
docker compose -f test/integration/docker-compose.yml up -d --wait
python3 test/integration/seed/seed.py                  # idempotent
pio run -e simulator
python3 test/integration/harness/run_scenarios.py

# one scenario, verbose
python3 test/integration/harness/run_scenarios.py --scenario sync_progress_pull -v
```

`docker compose ... down -v` resets the server completely; `seed.py` starts
from a database it recognizes as already seeded without duplicating anything.

## Continuous integration

The `integration` job in [ci.yml](../../.github/workflows/ci.yml) runs the same
chain on ubuntu: SDL2 + PlatformIO, the simulator built after applying
[platformio.local.example.ini](../../platformio.local.example.ini) (cold-build
include fix) and [ci/patch_simulator_package.py](ci/patch_simulator_package.py)
(HAL shims the fetched simulator package does not carry yet — the patcher is
idempotent and becomes a no-op as upstream catches up), then compose up, seed,
scenarios, compose down.

## Scenario inventory

| Scenario | What it proves |
| --- | --- |
| `sync_progress_pull` | A further server position (seeded by the synthetic device) is applied to both local progress homes. |
| `sync_progress_push` | A further local position reaches the server under the simulator's device id. |
| `highlight_pull` | A highlight created by the peer device lands in the local clipping store, with a minted position record. |
| `highlight_push` | A pre-seeded local highlight (clipping + position record, what the reader persists) reaches the server and is offered to a device that never saw it. |
| `highlight_delete_propagates` | Two runs on one SD: pull a highlight, delete its clipping locally, sync again — the complete key set deletes it server-side. |
| `highlight_delete_guard` | Same, but with the sync-state store lost between runs: the guard reports no key set and the server keeps the highlight. |
| `bookmark_pull` | A bookmark created by the peer lands in the local bookmark store, with a minted position record. |
| `bookmark_push` | A pre-seeded local bookmark reaches the server and is offered to a device that never saw it. |
| `catalog_collections_browse` | Scripted UI navigation: home menu → BookOrbit catalog → Collections → the seeded collection; asserted through the browser's list cache. The catalog flow never silent-reboots before exit, so input scripts drive it end to end. |
| `catalog_empty_listing_back` | Opening the seeded empty collection shows the no-entries error; Back climbs out of it and the next listing still loads (regression: Back reloaded the same empty listing forever). |

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
