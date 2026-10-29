---
title: Data Cache
nav_order: 16
---

# Data Cache

CrossInk caches data aggressively on the SD card to minimize RAM use. The ESP32-C3 has about 380 KB of usable RAM, so rebuilding every book structure in memory on every open would be too expensive.

The main data directory is `.crosspoint` on the SD card. It stores render caches and persistent user/device data.

## Directory Layout

```text
.crosspoint/
├── global_stats.bin        # All-time reading stats, including total books read
├── global_stats.bin.bak    # Backup used if the main global stats file is corrupt
├── synced_stats/           # Stats snapshots received from other readers
├── crossink-settings.json  # CrossInk device settings
├── settings.json           # Legacy settings fallback, if present
├── settings.bin.bak        # Legacy binary settings file after migration, if present
├── state.json              # Last-opened book and sleep/session state
├── state.bin.bak           # Legacy binary state file after migration, if present
├── recent.json             # Reading history for Home and Library
├── library.idx             # Library titles, authors, paths and sort indexes
├── library.meta            # Library EPUB metadata cache: slot table
├── library.metd            # Library EPUB metadata cache: parsed metadata records
├── recent.bin.bak          # Legacy binary recent-books file after migration, if present
├── wifi.json               # Saved Wi-Fi networks
├── opds.json               # Saved OPDS servers
├── koreader.json           # KOReader sync credentials
├── bookmarks/              # Bookmark files, one per book, named by content hash
├── clippings/              # EPUB clipping/highlight files, one per book, named by content hash
├── book_<contenthash>/     # Per-book BookOrbit sync state and reading-progress copy; follows the book through moves
├── bookorbit_downloads.bin # Where BookOrbit catalog downloads landed, for the catalog's on-device marker
├── bookorbit_download.json # A catalog download waiting out the X3/X4 restart before it; removed once read
├── home_carousel_cache_<index>.bin # Lyra Carousel artwork cache for each book position
├── sleep_frame.bin         # Temporary sleep overlay framebuffer, when used
├── epub_12471232/          # Each EPUB is cached to epub_<hash>
│   ├── progress.bin        # Reading position (chapter, page, etc.); also mirrored to book_<contenthash>/
│   ├── stats.bin           # Legacy per-book reading stats
│   ├── stats_v5.bin        # Version 5 per-book reading stats
│   ├── reading_stats_off   # Present when stats tracking is disabled for this book
│   ├── reader_settings.bin # Per-book reader settings, render mode, and auto-page-turn interval
│   ├── cover.bmp           # Book cover image, once generated
│   ├── cover_absolute.bmp  # Four-tone image-mode cover (generated separately)
│   ├── cover_crop_absolute.bmp # Cropped four-tone image-mode cover
│   ├── thumb_*.bmp         # Home/recent-books thumbnail images
│   ├── book.bin            # Book metadata, spine, table of contents, etc.
│   ├── css_rules.cache     # Parsed CSS rules
│   └── sections/           # Pre-rendered chapter/page layout data
│       ├── 0.bin
│       ├── 1.bin
│       └── ...
├── xtc_12471232/           # XTC progress and generated cover/thumb images
└── txt_12471232/           # TXT progress, page index, and generated cover image
```

Four-tone sleep covers use separate `_absolute.bmp` files so older cover shading is not reused. These are generated on demand for supported displays; TXT/Markdown JPG covers also use a separate `cover_absolute.bmp`. Existing cover and thumbnail caches remain available, and no manual cache reset is needed.

## Clearing Cache Data

Deleting the entire `.crosspoint` directory resets caches, settings, saved network/server data, bookmarks, recent books, reading progress, and reading stats.

To clear EPUB/XTC render caches from the device UI without deleting settings or global stats, use:

**Settings > System > Files & Cache > Clear Reading Cache**

## Book Moves And Cache Identity

Cache folders are path-based. Moving a book file can create a new cache directory, so the moved copy may start with fresh reading progress unless the firmware migrates the cache for that move. CrossInk migrates cache and bookmark data for the built-in move-to-Read flow and related file-browser move actions. Clippings, bookmarks and the `book_<contenthash>` state are named by the book's content hash instead, so they survive any move or rename, including ones made over USB or from a computer. Reading progress is written to both homes: the content-keyed copy follows the book through moves, while the path-keyed copy keeps stock CrossInk working after a firmware revert; on read, the further of the two positions wins.

EPUB reader font, page layout, styling, and reading-aid settings normally come from the global Reader settings. Changes made inside an EPUB override only the fields whose values differ from the global defaults; the other fields continue to inherit later global changes. EPUB render mode is stored separately per book so a problematic title can be switched to Balanced or Light rendering from the File Browser or Library long-press menus before opening it. Older full-snapshot book overrides retain their original behavior until reset or edited again.

EPUB clippings and highlights live outside the EPUB render-cache folder in
`/.crosspoint/clippings/`. Each book gets a binary clipping file named from the
book type and the CRC32 of the book path. The same clipping record powers the
in-reader highlight, the clipping list, and jump-back behavior. CrossInk also
appends a Kindle-style text export to `/My Clippings.txt` on the SD-card root;
that export is human-readable and append-only, so deleting a clipping in the UI
removes the in-app saved clipping but does not rewrite old text already exported
to `/My Clippings.txt`.

Cache data is cleared by supported CrossInk delete/move flows. If you remove or rename books outside CrossInk by editing the SD card directly, old cache folders may remain until you clear reading cache.

All-time reading stats can also be backed up outside `.crosspoint` in:

```text
/.crossink-stats-backup/
```

For binary file layout details, see [File Formats](./file-formats.md).

## Library

Library replaces the Recent Books screen.

The Library reconciles its index with the SD card on the first visit after a cold boot,
after file changes in File Browser, and after leaving a download or file-transfer
screen. Ordinary return visits and deep-sleep wakes reuse a successfully scanned
index. Failed or memory-limited scans are retried on the next visit. The Library's
refresh action always scans again; use it
if files were changed externally while the firmware stayed running or the reader
was asleep.
Unchanged books reuse their cached metadata. Each EPUB's parsed metadata is also
saved to `library.meta`/`library.metd` as soon as it is read, so a scan that is
cancelled, fails, or loses power picks up where it stopped next time instead of
parsing every book again. Deleting both files is safe; they are rebuilt as books
are parsed.
**Library > Settings > Use Book Metadata** selects embedded EPUB titles and
authors; disabling it uses filenames. TXT, Markdown and XTC files use filename
fallbacks. CLX1 version 2 adds a first-name author permutation; older Library
indexes rebuild automatically when Library opens.

**Date Added** uses the file creation time captured by the index. Its
first-seen sequence breaks ties and orders books without a creation time among
themselves; those books appear before dated books in ascending order.
The creation time is the filesystem's best available estimate of when a file
arrived on the card; some copy tools may preserve the original timestamp.
**Recently Opened** shows only books in the
saved reading history (up to 18 books), newest first. Reversing that sort shows
the same books oldest first. Marking a book unfinished does not add it to
reading history; opening it does.
Search matches words in the title and author and retains the selected sort.
The sort method and direction are saved when changed. The Library Settings icon
opens a compact/expanded list toggle and independent visibility switches for
EPUB, XTC/XTCH, TXT, and Markdown files. Expanded Title and Author sorts show
alphabetic headings; hidden file types remain indexed and can be shown again
without a rescan. Markdown books open as plain text in the TXT reader.
Expanded is the default for new Library preferences; a saved Compact choice remains respected.
Author (Last Name) sorts by the final word of the displayed author, while
Author (First Name) sorts by the displayed name from its beginning.
Series metadata and series sorting are not part of this UI change.

On button devices, Up from the first book reaches sort direction, sort method,
settings, search, and refresh. Confirm activates the selected control; holding Confirm on
a book opens its actions. On touch devices, tap the header icons or sort controls
directly and hold a book row for its actions. The sort method opens a modal list.

The Lyra Carousel snapshot cache advances to version 7 so cached home menus
regenerate with the Library label and landmark icon. No manual EPUB cache reset is required.
