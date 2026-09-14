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

- POSIX TZ signs are inverted from ISO 8601 in `TimeStore::applyTimezone()`: `"UTC-1"` means UTC+1.
- `LyraTheme::drawHeader()` does not call `BaseTheme::drawHeader()`, so header changes in the base theme must be duplicated in Lyra if needed.
