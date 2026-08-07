# Changelog

This repository is a fork of [CrossInk](https://github.com/uxjulia/CrossInk) and
records only its own additions. Each release states the CrossInk version it is based
on; for everything inherited from upstream, see the
[CrossInk changelog](https://github.com/uxjulia/CrossInk/blob/main/CHANGELOG.md).

## [Unreleased]

Based on CrossInk v1.5.0-rc-3.

### Changed

- A clipping saved with BookOrbit sync configured now stores the book's exact source text for the highlighted span (including the French non-breaking spaces before punctuation, kept as plain spaces). Clipping text used to be rebuilt from the rendered words, whose justified spacing could differ from the source ("mot ." for "mot.") -- visible in the clippings list and exports, and rejected by BookOrbit's text verification.
- Saved highlights and clippings keep up to 2048 bytes of text (previously 512), so long web-created highlights survive sync in full. Downgrading to an older release after saving a longer one makes that older firmware treat the book's clippings file as corrupt and drop it.

### Fixed

- Starting a BookOrbit or KOReader sync no longer crashes the device. Both ask an internet time server for the current time before syncing, and they did so from the wrong thread — harmless on earlier firmware, but the network stack CrossInk 1.5.0 builds against checks for this and aborts on the spot. It only happened when the time server's address was not already known, which is why a sync could work one minute and crash the next.
- Reopening a book returns to the page you left instead of the one before it. CrossInk 1.5.0 restores your position from a position in the text rather than a page number, so it survives a change of font or margins that repaginates the chapter — but the lookup stopped one page short whenever that position fell exactly on a page boundary, which is every time, since what gets saved is the start of the page you were on. Applying a position received from KOReader Sync was affected the same way.
- BookOrbit sync, statistics and the catalog work again against servers whose certificate chain has grown. Both had started failing at the TLS handshake: the certificate authorities now used by most hosts issue longer chains than this hardware can parse with the previous TLS library, whatever the free memory. All BookOrbit requests now use the same TLS transport CrossInk 1.5.0 introduced for its own downloads, which handles those chains comfortably — the catalog is also noticeably faster for it.

### Security

- BookOrbit connections no longer verify the server's certificate. The TLS transport that can complete these handshakes has no access to the root-certificate store the previous one used, so it cannot confirm that the server answering is really yours. Your credentials and reading data are still encrypted in transit; what is no longer checked is the identity at the other end, which matters on a network you do not control — a café or airport hotspot rather than your home WiFi. The same limitation applies to the OPDS catalog since CrossInk 1.5.0, and has always applied to KOReader Sync. It will be lifted once the SDK exposes a certificate store to that transport; until then, sync from a network you trust.

## [Unreleased]

Based on CrossInk v1.5.0-rc-3.

### Added

- Highlights now sync with BookOrbit, in both directions, on each BookOrbit sync of a book. Highlights made on the device appear in BookOrbit's web reader at their exact position; highlights created on the web come down and are drawn in the book; deletions propagate both ways. The sync screen reports what the exchange did ("Highlights: N sent, N added, N removed"). New highlights upload in batches of 8 per sync — this hardware bounds the payload that can share a TLS session — so a large backlog drains over a few syncs, and highlights made before this feature gain sync positions progressively as you read (one per chapter visited). A highlight's stored text is capped at 2048 bytes, so the drawn span of an extremely long web highlight ends where that cap cuts its text.

### Fixed

- Saved highlights are drawn again after a font or layout change in real-world text. The reader re-finds a highlight by matching its text word-by-word against the page, but the layout splits punctuation and hyphens into words of their own ("toilettes", ".") and hyphenation splits words at line breaks ("Bi-", "zarre"), so any highlight containing punctuation — in French, nearly all of them — silently stopped being drawn. A character-level match that ignores whitespace and hyphens on both sides now takes over when the word-by-word one fails, and it also draws highlights that span a page turn on both of their pages.

## [v1.4.1+bookorbit.3] - 2026-08-02

Based on CrossInk v1.4.0.

### Fixed

- Firmware updates from the device work again. Every release of this fork carries its number in the part of the version string semver reserves for build metadata, which is defined to carry no precedence, so the updater read two consecutive releases as the same version and reported "Update failed" instead of offering the update. Devices already running an earlier build still need one manual install to pick this up; updates from the device work from there on.
- The last entry of the home menu can be selected again on the Classic and Lyra themes. Navigation wrapped using a count maintained separately from the menu itself, and that count was never updated when the BookOrbit entry was added, so it stopped one position short — leaving Settings, which comes last, impossible to reach.

## [v1.4.1+bookorbit.2] - 2026-08-02

Based on CrossInk v1.4.0.

### Added

- The clock in the top status bar now works on devices without a clock chip (the X4), reading the system clock. The clock is refreshed from NTP on every WiFi connection on those devices. Hide Clock, Clock Format and Clock UTC Offset are all reachable now — they were hidden without a real RTC, which made the clock impossible to enable or configure.
- Reading activity is now uploaded to BookOrbit's reading-statistics dashboard on each BookOrbit sync, as per-page reading events like BookOrbit's own KOReader plugin sends — feeding its reading time, streaks, pace and reading-DNA stats with full granularity (BookOrbit's API is upload-only, so stats flow from CrossInk to BookOrbit).
- Reading-session timestamps are corrected retroactively at sync time, using the clock error each sync measures. When the clock ran continuously since the previous sync, the error is drift and is spread across the sessions in proportion to when they happened; when the clock had been lost and restored, the whole block of sessions is shifted instead, keeping the gaps between them intact. Either way, sessions land at the right moment in BookOrbit's dashboard instead of at the moment they were uploaded.
- "Keep Clock While Asleep" (Settings > System > Device, only shown on devices without a clock chip): keeps the board powered during sleep so wall-clock time keeps running instead of being lost at every sleep. Off by default, because it trades standby battery life for that: with it enabled the device no longer powers off completely when it sleeps and keeps drawing a small current, so it slowly discharges even when unused. Watch your own battery for a few days before relying on it. Turning the setting off restores the previous behaviour exactly.

### Fixed

- BookOrbit authentication now releases the settings screens it was opened from before connecting. Those screens hold 15-20 KB, and on this hardware a TLS handshake completes with only a few KB to spare, so authentication could fail outright while the same server answered fine from the catalog — which already worked this way. Nothing changes on screen: authenticating brought WiFi up, and that has always ended in a silent restart to the home screen.
- The clock's NTP refresh no longer leaves the SNTP client running once it has read the time. On devices without a clock chip that refresh happens on every WiFi connection, immediately before whatever request you connected for, and the idle client held a socket and its buffers for the rest of the session — memory the following HTTPS connection needs on hardware this tight.
- Settings is reachable again from the home menu. The home menu held at most eight entries and silently dropped anything beyond that; adding BookOrbit made nine on devices that also have OPDS servers, reading stats and bookmarks, and Settings — pushed last — was the one dropped. It was missing from the list entirely, which is why scrolling never reached it.
- The reader's top clock and the first line of book text no longer touch: the text now keeps a few pixels of clearance below the clock band.
- The BookOrbit stats upload no longer builds its `pluginVersion` from the firmware version string: the server caps that field at 20 characters and rejected the whole upload from `xlarge` and dev builds (finding adopted from the samfoy/CrossInk fork, verified against a live server).
- BookOrbit servers that predate the page-stats endpoint no longer cause queued stats to retry forever: the queue self-heals (drops with a log) on HTTP 404/405/501.

## [v1.4.1+bookorbit.1] - 2026-07-31

Based on CrossInk v1.4.0.

### Added

- BookOrbit Sync, an alternative reading-progress sync provider alongside KOReader Sync: configure your account under Settings > BookOrbit Sync, then sync a book from the reader menu. It can also be assigned to the power button (short or long press) or to long-press Menu/Back in the reader, like KOReader's Sync Progress.
- A BookOrbit catalog browser for finding and downloading EPUBs from your server, reachable from Settings > BookOrbit Sync > Browse Catalog or from a BookOrbit entry in the home menu. It lists the server's own sections, browses books by author or by series, searches the library, and adds two offline categories: "On device" (EPUBs already in the download and /Read folders) and "In progress" (recent books you haven't finished) — picking one of those opens the book in the reader. Books already present on the device are marked with a dot at the end of the line.

### Fixed

- Downloads (BookOrbit and OPDS) allocate their transfer buffer before the TLS handshake instead of at the post-handshake heap low point, so they no longer fail to start when memory is tight, and pressing Back during the connection phase now cancels instead of being ignored until the first bytes arrive.
