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
# one-time / when fixtures change
python3 test/integration/seed/make_library.py

# start + seed the server (idempotent)
docker compose -f test/integration/docker-compose.yml up -d
python3 test/integration/seed/seed.py

# build the simulator and run every scenario
pio run -e simulator
python3 test/integration/harness/run_scenarios.py

# one scenario, verbose
python3 test/integration/harness/run_scenarios.py --scenario sync_progress_pull -v
```

`docker compose ... down -v` resets the server completely; `seed.py` starts
from a database it recognizes as already seeded without duplicating anything.

## Scenario inventory

| Scenario | What it proves |
| --- | --- |
| `sync_progress_pull` | A further server position (seeded by the synthetic device) is applied to both local progress homes. |
| `sync_progress_push` | A further local position reaches the server under the simulator's device id. |

Planned next (see the tracking notes in the harness): highlight pull/push,
highlight deletion propagating (and the missing-store guard *not*
propagating), bookmark exchange, catalog download into the configured folder
plus on-device marker, moved-book content-hash continuity, two-device
round-trips through the synthetic device.

## Notes

- The firmware side is driven through seeded settings (power shortcut =
  BookOrbit Sync, Smart sync behavior, `state.json` naming the open book) plus
  one `CROSSPOINT_SIM_INPUT_SCRIPT` power press; everything after the network
  reboot is driven by the firmware-side hooks above.
- Fixture books have 5 spine items; positions seeded beyond the spine range
  are clamped back to the beginning by the sync's own guard.
- Endpoints and payloads mirror `lib/BookOrbitSync/BookOrbitSyncClient.cpp`;
  when the protocol evolves there, evolve `seed/kosync.py` with it.
