# Changelog

This repository is a fork of [CrossInk](https://github.com/uxjulia/CrossInk) and
records only its own additions. Each release states the CrossInk version it is based
on; for everything inherited from upstream, see the
[CrossInk changelog](https://github.com/uxjulia/CrossInk/blob/main/CHANGELOG.md).

## [Unreleased]

### Fixed

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
