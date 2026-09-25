# CrossPoint Reader — Durable Context

Keep this file focused on repo-specific gotchas that are worth reusing in future sessions.

## FreeInk SDK

Refer to https://freeink.org/llms.txt for guidance.

## Simulator

- Simulator patches belong in the adjacent `crossink-simulator` repo (fetched from `github.com/uxjulia/crossink-simulator` into `.pio/libdeps/simulator/simulator` when no local checkout is symlinked).
- The valid local simulator env in this repo is `simulator`.
- After the upstream sync (9c7315f4), the published simulator stubs are missing symbols the firmware now uses: `WiFi.disconnect(bool, bool, timeout)` returning `bool`, `HWCDC::read(buf, len)`, `HWCDC::setRx/TxBufferSize`, `HalStorage::installDateTimeCallback`, and `vSemaphoreDelete` in the FreeRTOS shim. These were patched locally in `.pio/libdeps` (wiped by a clean); the durable fix is stub additions in crossink-simulator.
- The simulator `PNGdec` stub in `crossink-simulator/src/PNGdec.h` needs to mirror the real API shape used by app code, including `hasAlpha()` and `getTransparentColor()`, even though decode still fails intentionally.
- Known simulator limits:
  - No image rendering: `platformio.ini` ignores `hal`, `PNGdec`, and `JPEGDEC`, so image decoders are intentionally absent.
  - JPEGDEC stub always fails; `JPEGDEC fallback: open failed (err=-1)` is expected in simulator.
  - `esp_deep_sleep_start()` is a no-op in simulator.
  - `HalStorage` uses POSIX file access under `./fs_` and allows multiple readers, unlike real hardware.

## Real Hardware / Storage

- SdFat on hardware allows only one open reader per file path at a time. If a fallback needs to reopen the same file, close the first handle before reopening.

## Rendering / Reader Pipeline

- `lib/Epub/Epub/Page.cpp`: images must render only in `GfxRenderer::BW`; grayscale passes are text anti-aliasing passes only.
- Kindle EPUBs may contain paired high-res and old-Kindle fallback images. `ChapterHtmlSlimParser` should skip `<img>` nodes with `data-AmznRemoved-M8` to avoid duplicate stacked images.
- After image/layout pipeline changes that affect cached EPUB output, clear the affected `.crosspoint/epub_<hash>/` cache if behavior looks stale.

## UI Consistency

- Use FreeInkUI SDK components and input routing for list-style screens where possible. Row rendering, touch targets,
  hit testing, and pagination should share the same FreeInkUI list configuration instead of custom touch scaling.

## Heap Baselines (X4 hardware, SD card font)

- A normal resume-into-partial reading session runs at ~85-90KB free / ~49KB maxAlloc by
  the first watermark crossing (Epub metadata + x-locations + resident glyph caches).
  Do not read mid-range heap numbers as session degradation without checking the scenario.
- SD-font section builds cost ~38-50KB at cold start; the 4-style advance-table prewarm
  (~30KB incl. 16KB contiguous scratch) dominates and is skipped below 80KB free.

## Networking / Memory

- `HttpDownloader::fetchUrl(std::string&)` buffers the whole response in RAM. std::string growth uses the throwing `operator new`, which aborts on OOM with `-fno-exceptions` — this crashed the device on large BookOrbit catalog responses (v1.4.0-dev). The overload now has a max-alloc heap guard, but for API JSON of unbounded size prefer `downloadToFile` to SD + `deserializeJson` from the file with a `DeserializationOption::Filter` (see BookOrbitCatalogClient::fetchJson).
- Measured on X4 hardware (2026-07): active WiFi leaves ~65KB free; an HTTPS session (esp_http_client + crt bundle) costs ~54KB through the handshake. mbedTLS then allocates workspace per incoming TLS record (up to 16KB), so post-handshake `getMaxAllocHeap()` must stay ≥ ~16KB or reads fail mid-body against servers that send large records. Consequences: allocate transfer buffers *before* opening the connection, and launch network-heavy activities via `replaceActivity` (clears the whole activity stack — the settings screens alone hold 15-20KB) rather than pushing on top of it. `runGet` logs "Before client init"/"After open (TLS up)" heap breadcrumbs at INF for field diagnosis.

## Misc Repo Gotchas

- On the X4, `RTC_NOINIT_ATTR` data does NOT survive deep sleep (every wake read as garbage), while the system clock itself DOES survive both deep sleep and software resets (RTS/EN flash resets included). Never key state to RTC memory across sleeps; persist to SD and use clock plausibility for cold-boot detection (see lib/WallClock).
- Never range-for over a ternary of `std::initializer_list` temporaries (`for (x : cond ? std::initializer_list<T>{} : std::initializer_list<T>{a, b})`). The backing array's lifetime is NOT extended through `?:`, GCC 14 (-Os, `-Wdangling-pointer`) drops the stores into it as dead and the loop reads uninitialized stack. This is how the fork's `keepClockInSleep` gate silently disabled the X4 GPIO13 battery-latch release for every value of the flag (v1.5.0+bookorbit.1 → 2026-09-13, now a plain `if` around the loop): the board never powered off in sleep, the clock kept running, standby drained. Only the direct form `for (x : {a, b})` is safe; gate with an `if` around the loop instead.
- `WallClock` eras are what make retroactive timestamp correction sound: a clock loss always increments the era BEFORE the checkpoint is restored, so within one era the clock can only have drifted, never stepped. That is why a same-era correction may be interpolated between two syncs, while an era that opened on a clock loss takes its measured error as a flat shift — its error starts at the unknown powered-off duration, not at zero. Do not blend the two.

- Upstream squashes each release onto `main`, so `git merge v1.X.Y` computes its base at the previous release and reports hundreds of phantom conflicts. Graft the tag locally onto the last rc the fork was aligned on first (`git replace --graft <tag> <tag^> <rc>`), merge, then `git replace -d <tag>`: the merge commit still records the real tag SHA (v1.5.1: 26 real conflicts instead of ~200). Never resolve a conflicted file with whole-file `--theirs`; fork commits are never ancestors of the upstream side, so that erases every fork edit in the file. Use `git checkout -m -- <file>` and take theirs hunk by hunk. The graft is only needed when the fork last aligned on an rc: after merging the real vX.Y.Z tag, `git merge-base main <next tag>` already is that tag (v1.6.0: 14 conflicts, no graft).
- `HttpDownloader::DownloadOptions::extraHeaders` carry BookOrbit's `x-auth-*` keys, so they are credentials: they only go to `authorizationOrigin` (default: the request URL), like Basic auth. Keep that gating when touching redirects.
- Integration seed: `ensure_collection` reuses an existing server collection by name without re-syncing its members, so after `library.json` changes the catalog scenarios fail on a shifted book set. `docker compose -f test/integration/docker-compose.yml down -v` and reseed before suspecting the firmware.
- The integration BookOrbit image ships BookOrbit's own KOReader plugin at `/app/koreader-plugin/bookorbit.koplugin` (`docker exec`-readable Lua). It is the reference for catalog behaviour — `bookorbit_catalog.lua`'s `paramsForEntry` is the authoritative section-id -> books-query mapping — and the server's DTOs are in `/app/dist` and `/app/node_modules/@bookorbit/types`. Read those instead of guessing an endpoint's shape; an unknown query param is silently ignored, so a wrong guess looks like a working filter.
- The catalog is EPUB-only, so every books listing sends `format=epub` (filtered server-side before paging). Only `/plugin/catalog/books` takes that filter: the dashboard's `totalBooks`/`inProgress` and the `/sections/*` per-entry counts cover every format, so book counts shown for a listing must come from that listing's own `total` (a `size=1` request).
- The catalog's root section order comes from the server, and the browser drops sections it cannot browse. Making a new section browsable therefore shifts every row below it, which breaks the integration scenarios that navigate the root by counting DOWN presses.
- Catalog scenarios flake when the integration server is busy scanning book metadata: listings come back empty and `devicePath` loses its `{authors:first}` folder, so a whole batch can fail at once while the server still reports healthy. Re-run before suspecting the firmware; book titles also read as filename stems until the scan catches up.
- POSIX TZ signs are inverted from ISO 8601 in `TimeStore::applyTimezone()`: `"UTC-1"` means UTC+1.
- `LyraTheme::drawHeader()` does not call `BaseTheme::drawHeader()`, so header changes in the base theme must be duplicated in Lyra if needed.
