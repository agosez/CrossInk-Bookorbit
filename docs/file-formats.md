# File Formats

These formats describe the SD-card cache files under `/.crosspoint/epub_<hash>/`.
All POD fields are written in the ESP32 little-endian representation used by
`Serialization.h`; strings are length-prefixed UTF-8 unless a format notes a
fixed-size char buffer.

## `/.crosspoint/sleep-image-index/<directory-hash>-{bmp,all}.idx`

### Version 1

Sleep screens keep a compact, rebuildable index for the selected sleep-image
folder. The index avoids walking the directory during every sleep while using
only one fixed-size record at a time in RAM. `bmp` contains BMP files and
`all` contains BMP and PNG files for Page Overlay mode. The `validated` header
flag means BMP headers were checked while rebuilding after a failed render.

The index is disposable: a missing, malformed, or stale selected entry causes
one rebuild and then the sleep renderer falls back to its directory scan. File
transfer, file-browser, and preferred-folder changes invalidate affected
indexes. Files added or changed directly on the SD card have no notification
path; they are picked up when a cached entry is found missing or when an index
is otherwise rebuilt.

```c++
struct SleepImageIndexHeader {
    char magic[4];       // "CSIX"
    u8 version;          // 1
    u8 flags;            // bit 0: BMP+PNG, bit 1: BMP headers validated
    u16 pathLength;
    u16 recordCount;
    u16 recordSize;      // sizeof(SleepImageIndexRecord)
    u32 recordsOffset;   // sizeof(header) + pathLength
    char directory[pathLength];
};

struct SleepImageIndexRecord {
    u16 nameLength;
    u8 flags;             // bit 0: PNG (otherwise BMP)
    u8 reserved;
    char name[256];      // zero-padded UTF-8 filename, max 255 bytes
};
```

## `book.bin`

### Version 9

`book.bin` stores EPUB metadata plus lookup tables for spine and TOC entries.
The current firmware writes this version from `BookMetadataCache`.
Version 9 stores book and TOC title strings NFC-composed so decomposed
diacritics render correctly with device fonts. It also rebuilds metadata after
the EPUB guide start-reference handling changed.

ImHex pattern:

```c++
import std.mem;
import std.string;
import std.core;

#define EXPECTED_VERSION 9
#define MAX_STRING_LENGTH 65535

struct String {
    u32 length [[hidden, comment("String byte length")]];
    if (length > MAX_STRING_LENGTH) {
        std::warning(std::format("Unusually large string length: {} bytes", length));
    }
    char data[length] [[comment("UTF-8 string data")]];
} [[sealed, format("format_string"), comment("Length-prefixed UTF-8 string")]];

fn format_string(String s) {
    return s.data;
};

struct Metadata {
    String title [[comment("Book title")]];
    String author [[comment("Book author")]];
    String language [[comment("Book language code")]];
    String coverItemHref [[comment("Path to cover image")]];
    String textReferenceHref [[comment("Path to guided first text reference")]];
};

struct SpineEntry {
    String href [[comment("Resource path")]];
    u32 cumulativeSize [[comment("Cumulative uncompressed spine size through this entry")]];
    s16 tocIndex [[comment("Index into TOC, or inherited/previous TOC index when no direct entry exists")]];
};

struct TocEntry {
    String title [[comment("Chapter/section title")]];
    String href [[comment("Resource path")]];
    String anchor [[comment("Fragment identifier")]];
    u8 level [[comment("Nesting level")]];
    s16 spineIndex [[comment("Index into spine (-1 if none)")]];
};

struct BookBin {
    u8 version;
    if (version != EXPECTED_VERSION) {
        std::error(std::format("Unsupported version: {} (expected {})", version, EXPECTED_VERSION));
    }

    u32 lutOffset [[comment("Offset to lookup tables")]];
    u16 spineCount;
    u16 tocCount;

    Metadata metadata;

    u32 currentOffset = $;
    if (currentOffset != lutOffset) {
        std::warning(std::format("LUT offset mismatch: expected 0x{:X}, got 0x{:X}", lutOffset, currentOffset));
    }

    u32 spineLut[spineCount] [[comment("Spine entry offsets")]];
    u32 tocLut[tocCount] [[comment("TOC entry offsets")]];

    SpineEntry spines[spineCount];
    TocEntry toc[tocCount];
};

BookBin book @ 0x00;

u32 fileSize = std::mem::size();
u32 parsedSize = $;
if (parsedSize != fileSize) {
    std::warning(std::format("Unparsed data detected: {} bytes remaining at offset 0x{:X}", fileSize - parsedSize, parsedSize));
}
```

## `reader_settings.bin`

### Version 9

Each EPUB cache directory may contain `reader_settings.bin`. Missing files mean
the book uses global Reader settings and the default auto-page-turn interval.

Version 1 stored only:

- `u8 version`
- `u16 autoPageTurnSeconds`

Version 2 stores flags before the full reader-settings snapshot. Version 3 adds
the EPUB word-spacing level to that snapshot. Version 4 adds the EPUB indexing
method (`0` = incremental, `1` = full section). Version 5 appends a per-book
dictionary SD-font family name. Version 6 stores reader font sizes as physical
point sizes, version 7 appends the dictionary font's selected point size, and
version 8 splits the screen margin into vertical and horizontal values. Version
9 removes the obsolete per-book Dark Mode byte: Dark Mode is now a global
display setting.
This lets the
file preserve an auto-page-turn interval without forcing custom font/layout
settings for the book. It also stores a per-book EPUB render mode override,
which can be changed from book action menus before opening the book so a
problematic EPUB can be moved to Balanced or Light rendering without entering
the reader first. Safe Mode also uses this file to save Light rendering with
embedded styles, Bionic Reading, and Guide Dots disabled after that final
fallback successfully opens a difficult book.

```c++
struct ReaderSettingsBin {
    u8 version; // 9
    u8 flags;   // bit 0 = custom reader settings, bit 1 = custom auto-page-turn interval, bit 2 = render mode override, bit 3 = dictionary font override
    u16 autoPageTurnSeconds;
    u8 renderMode; // 0 = CrossInk Default, 1 = Balanced, 2 = Light

    u8 fontFamily;
    u8 readerFontPointSize; // physical point size; versions 2-5 stored a size slot
    u8 lineHeightPercent;
    u8 wordSpacing; // 0 = natural font spacing; 1-4 widen each gap by ~75% per level
    u8 orientation;
    u8 screenMarginVertical;
    u8 screenMarginHorizontal;
    u8 publisherPageNumbers;
    u8 paragraphAlignment;
    u8 embeddedStyle;
    u8 hyphenationEnabled;
    u8 textAntiAliasing;
    u8 imageRendering;
    u8 extraParagraphSpacing;
    u8 forceParagraphIndents;
    u8 bionicReadingEnabled;
    u8 guideReadingEnabled;
    u8 snapshotRenderMode;
    u8 indexingMethod; // 0 = incremental, 1 = full section
    char sdFontFamilyName[64];
    char dictionarySdFontFamilyName[64]; // meaningful only when flag bit 3 is set
    u8 dictionaryFontPointSize; // 0 = follow reader size
};
```

## `/.crosspoint/clippings/<bookType>_<contentHash>.bin`

### Versions 1-3

Clipping files store the per-book EPUB clipping list used by the reader. A
saved clipping is also what CrossInk renders as an in-reader highlight; there is
no separate highlight file. The file lives in `/.crosspoint/clippings/` instead
of the EPUB render-cache directory so clearing/rebuilding layout cache does not
delete user clippings.

The current implementation only writes EPUB clipping files, so `bookType` is
`epub`. The suffix is the book's 32-hex partial-MD5 content hash — the same
identity BookOrbit sync reports to the server — so the file follows the book
through moves and renames. Files written by earlier releases used
`uzlib_crc32()` of the book's SD-card path as a decimal suffix; `loadForBook`
renames such a file to the content-keyed name the first time the book is
loaded, and keeps using the path-keyed name only when the book file itself
cannot be read. The same applies to bookmark files under
`/.crosspoint/bookmarks/`, which additionally retain their older
`std::hash`-suffixed tier as a merged-in legacy. Example:

```text
/.crosspoint/clippings/epub_8f14e45fceea167a5a36dedd4bea2543.bin
```

Binary layout:

- `[0]` version (`1`, `2`, or current version `3`)
- `[1-2]` clipping count (`uint16_t` LE, maximum `256`)
- book title (`String`)
- book author (`String`)
- book path (`String`)
- repeated clipping records:
  - `spineIndex` (`uint16_t` LE)
  - `startPage` (`uint16_t` LE)
  - `endPage` (`uint16_t` LE)
  - `pageCount` (`uint16_t` LE, at least `1`)
  - `startWordIndex` (`uint16_t` LE)
  - `endWordIndex` (`uint16_t` LE)
  - `wordCount` (`uint16_t` LE)
  - `paragraphIndex` (`uint16_t` LE, `UINT16_MAX` when unavailable)
  - `timestamp` (`uint32_t` LE, seconds since firmware boot when saved)
  - version 3 only: reader layout signature (`uint32_t` LE; font, spacing,
    viewport, and other section-layout inputs)
  - `chapterTitle` (`char[48]`, null-terminated/truncated)
  - version 1: selected text (`String`; legacy files were written with a
    `512`-byte in-app limit)
  - versions 2-3: selected-text length (`uint16_t` LE) followed by that many
    UTF-8 bytes (the current in-app limit is `4096` bytes, defined by
    `CLIPPING_TEXT_MAX`; builds with a smaller historical cap treat a longer
    record as corrupt on read, so downgrading loses long highlights)

The clipping selector has a separate navigation bound: it exposes at most
`240` visible words from at most three pages. This is a bounded in-memory
selection window for low-memory devices, not a character-count limit. The
selected text is still stored separately and is limited to `4096` UTF-8 bytes.

CrossInk uses the stored spine/page/paragraph fields as anchors, then searches
near that location for the stored clipping text after relayout. This is similar
to keeping both a DOM position and a text quote in a web app: the numeric
position gives a fast starting point, while the text makes jumps and highlights
survive font, layout, or page-count changes when possible.

Version 3 records which reader layout produced the numeric page/word anchor.
When that signature differs, CrossInk ignores the stale numeric range and
matches the saved text instead, including when both layouts happen to have the
same total page count. Versions 1-2 retain their numeric fast path until the
reader sees a relayout, when it stamps the previously active layout before
rebuilding.

Creating a clipping also appends a Kindle-style export entry to
`/My Clippings.txt` on the SD-card root. That text export can keep up to `2000`
bytes of the selected text and is append-only. Removing a clipping from the
reader deletes or rewrites only the binary clipping file; it does not remove
previous entries from `/My Clippings.txt`.

When CrossInk moves an EPUB through its built-in move-to-Read flow, it rewrites
the clipping file under the new path-derived name and removes the old one. If a
book is renamed or moved outside CrossInk, the path hash changes, so the old
clipping file may no longer be associated with the book until the file is moved
back or the clipping store is migrated.

## `stats_v5.bin`

### Version 5

`stats_v5.bin` stores per-book reading statistics for stats schema version 5.
Versioned filenames let firmware branches with different stats schemas keep
their own per-book stats files without overwriting each other. Version 5 extends
version 4 with a cached live reader book time-left estimate so Home and Reading
Stats can show the same estimate the reader last computed.

When `stats_v5.bin` is missing, CrossInk can read the previous versioned stats
filename (`stats_v4.bin` for version 5, `stats_v5.bin` after a future version 6
bump) before falling back to legacy `stats.bin` files with compatible stats
payloads. Future changes are always saved to the current versioned filename.

Binary layout:

- `[0]` version (`5`)
- `[1-2]` `sessionCount` (`uint16_t` LE)
- `[3-6]` `totalReadingSeconds` (`uint32_t` LE)
- `[7-10]` `totalPagesTurned` (`uint32_t` LE)
- `[11]` `isCompleted` (`uint8_t`)
- `[12-13]` `avgSecondsPerForwardPage` (`uint16_t` LE)
- `[14-15]` `paceSampleCount` (`uint16_t` LE)
- `[16]` flags (`bit0=startDateManual`, `bit1=finishedDateManual`)
- `[17-20]` `startDate` (`year uint16_t` LE, `month uint8_t`, `day uint8_t`)
- `[21-24]` `finishedDate` (`year uint16_t` LE, `month uint8_t`, `day uint8_t`)
- `[25-40]` `timeOfDaySeconds[4]` (`uint32_t` LE each)
- `[41-68]` `dayOfWeekSeconds[7]` (`uint32_t` LE each)
- `[69-72]` `estimatedTimeLeftSeconds` (`uint32_t` LE, `0` means unavailable)

## `bookorbit_annotations.bin`

### Version 1

`bookorbit_annotations.bin` lives in the book's content-keyed state directory,
`/.crosspoint/book_<contentHash>/`, together with `bookorbit_bookmarks.bin`,
`bookorbit_stats.bin` and `bookorbit_sync.bin`, so all of it follows the book through
moves and renames. Earlier releases kept these files in the book's path-keyed render
cache directory; the first sync or position mint after the update moves them over. The
file holds what BookOrbit needs to identify each highlight: the KOReader xpointer the
server keys on, plus the upload watermark.

The xpointers are minted when the highlight is created, not when it is synced. Building
them streams and parses the whole chapter (`ChapterXPathResolver`), which is nearly free
while the reader already has that chapter open but would mean parsing every affected
chapter with 55 KB already committed to a TLS handshake if it were left to sync time.
Minting once also keeps identity stable: the server keys annotations by
`md5(datetime | pos0)`, and a position rebuilt under a different layout would make every
highlight reappear as new.

`pos0` and `pos1` delimit the highlighted text, `pos1` exclusive, as KOReader does.
A paragraph-level position is not a usable substitute: a server that resolves the range
and reads the text back finds nothing in an empty range and flags the annotation as
repaired, which reads to the user as a failure. Offsets count codepoints in the
whitespace-collapsed view of a single text node -- the crengine convention BookOrbit
resolves against, where whitespace runs (Unicode spaces included) become one space --
so a highlight starting inside inline markup gets that element's node
(`/p[4]/em[1]/text()[1].3`) rather than the paragraph's.

Minting a precise position also replaces the clipping's stored text with the matched
span's exact source text: the clipping was built from rendered words, whose justified
spacing around detached punctuation differs from the source, and the server rejects an
upload whose text does not read back from its own copy.

When the stored text no longer matches the chapter -- hyphenation, an entity that decoded
differently, an edited book -- the reader falls back to a paragraph-level range and logs
it, rather than writing an offset it cannot justify.

Records join back to their `Clipping` by `timestamp` **plus** `spineIndex` and
`paragraphIndex`. `Clipping::timestamp` is `millis()/1000` — seconds since boot, not a date —
so two highlights made at the same offset in different sessions share it; the chapter and
paragraph make a remaining collision mean the same highlight in practice.

`identityEpoch` is the datetime the server hashes into the annotation's key, and it is a real
UTC epoch taken from `wallclock.bin`. It is deliberately *not* derived from `timestamp`:
seconds-since-boot dated every highlight 1970 on the server and made the upload watermark
non-monotonic, so a highlight created early in a session looked older than one from a previous
session and was skipped as already sent. For a highlight received *from* the server it holds
that server's own datetime, without which the server saw a foreign key, re-offered the
highlight on every sync, and never reported its deletion.

Kept separate from the clipping format because several screens read that one: highlight
sync can gain fields without any of them caring. A file whose magic does not match is
discarded on read and rewritten on the next write, costing only the minted xpointers,
which the reader re-stamps as chapters are opened. Capped at 256 records, matching
`CLIPPING_MAX_PER_BOOK`.

Binary layout (all little-endian):

- `[0-3]` magic + version: ASCII `BOA1`
- `[4-7]` `watermark` (`uint32_t`, newest highlight timestamp the server has accepted, `0` when nothing has been sent)
- Repeated variable-length records:
  - `[0-3]` `timestamp` (`uint32_t`, `Clipping::timestamp`, seconds since boot; part of the join key)
  - `[4-7]` `identityEpoch` (`uint32_t`, the datetime the server keys on, real UTC epoch seconds)
  - `[8-9]` `spineIndex` (`uint16_t`)
  - `[10-11]` `paragraphIndex` (`uint16_t`)
  - `[12-13]` `pos0Length` (`uint16_t`, 1-512)
  - `[14-15]` `pos1Length` (`uint16_t`, 1-512)
  - `[16…]` `pos0` then `pos1` (that many bytes each, KOReader xpointers, not null-terminated)

## `bookorbit_bookmarks.bin`

### Version 1

`bookorbit_bookmarks.bin` lives beside `bookorbit_annotations.bin` in the book's
content-keyed state directory (`/.crosspoint/book_<contentHash>/`) and holds what BookOrbit
needs to identify each bookmark: the KOReader xpointer the server keys on
(`md5(datetime | pos)`), plus the upload watermark. Positions are minted when the
bookmark is created, from the page's first visible codepoint (the layout-independent
coordinate the section cache stores per page), and never recomputed.

Records join back to their `Bookmark` by `timestamp` plus `spineIndex`; `timestamp` is a
real UTC epoch, stamped by `BookmarkStore::addBookmark` from WallClock (bookmarks from
older builds carry 0 and gain a timestamp through the reader's backfill). `identityEpoch`
is the datetime the server keys on: equal to `timestamp` for a bookmark made on the
device, and the identity this device MINTED at apply time for one received from the
server -- bookmarks invert the annotation convention, the device owns the identity and
reports it in the exchange acknowledgment.

Binary layout (all little-endian):

- `[0-3]` magic + version: ASCII `BOB1`
- `[4-7]` `watermark` (`uint32_t`, newest identityEpoch the server has accepted)
- Repeated variable-length records:
  - `[0-3]` `timestamp` (`uint32_t`, the bookmark's creation epoch; part of the join key)
  - `[4-7]` `identityEpoch` (`uint32_t`)
  - `[8-9]` `spineIndex` (`uint16_t`)
  - `[10-11]` `posLength` (`uint16_t`, 1-512)
  - `[12…]` `pos` (`posLength` bytes, KOReader xpointer, not null-terminated)

## `bookorbit_stats.bin`

### Version 1

`bookorbit_stats.bin` is a queue of per-page reading events in a book's
content-keyed state directory (`/.crosspoint/book_<contentHash>/`), waiting to
be uploaded to a BookOrbit server's page-stats endpoint
(the server clusters raw page events into the reading sessions that power its
time/streak/pace stats). The reader buffers one record per qualifying forward
page read in RAM and appends them as a single batch when the session ends;
BookOrbitSyncActivity uploads and deletes the file on the next successful
BookOrbit sync of that book. The queue is capped at 2000 records; on overflow
the oldest records are dropped.

Each record carries the WallClock power era its timestamp was taken in, plus a
flag marking the timestamp approximate (taken from the system clock rather than a
battery-backed RTC), so it can be corrected to real time at upload — see
`wallclock.bin`. A queue whose header does not match is discarded on read and
replaced on append.

Binary layout (all little-endian):

- `[0-3]` magic + version: ASCII `BOQ1`
- Repeated 16-byte records:
  - `[0-3]` `startTime` (`uint32_t`, page-read start as UTC epoch seconds, possibly approximate)
  - `[4-7]` `durationSeconds` (`uint32_t`, dwell seconds on the page)
  - `[8-9]` `page` (`uint16_t`, overall book position in basis points 0-10000)
  - `[10-11]` `totalPages` (`uint16_t`, the position denominator, `10000`)
  - `[12-13]` `era` (`uint16_t`, truncated WallClock power era the timestamp was taken in)
  - `[14]` `flags` (`uint8_t`, bit0 = clock was approximate / not NTP-confirmed)
  - `[15]` reserved

## `/.crosspoint/bookorbit_downloads.bin`

### Version 1

Remembers where BookOrbit catalog downloads landed on the SD card, keyed by the
server's book id, so the catalog can mark a book as already on the device even
when the download folder setting has since changed or the file was renamed
through a supported flow. It is a best-effort convenience cache: entries are
verified on lookup (the file must still exist with its recorded size) and
dropped when stale, and a missing or discarded index only costs the marker, with
the filename heuristic as fallback. Capped at 128 entries, oldest evicted first.

Book ids are only meaningful on the server that issued them, so the header
records a CRC32 of the configured server URL; the whole file is discarded when
it no longer matches, as it is when the magic is unreadable.

Binary layout (all little-endian):

- `[0-3]` magic + version: ASCII `BOD1`
- `[4-7]` `serverCrc` (`uint32_t`, CRC32 of the configured BookOrbit server URL)
- Repeated variable-length records:
  - `[0-7]` `bookId` (`int64_t`, the server's book id)
  - `[8-11]` `fileSize` (`uint32_t`, size of the downloaded file, for the replacement check)
  - `[12-13]` `pathLength` (`uint16_t`, 1-256)
  - `[14…]` `path` (`pathLength` bytes, absolute SD path, not null-terminated)

## `/.crosspoint/wallclock.bin`

### Version 1

WallClock's persisted state for devices without a battery-backed RTC: the
clock-timeline counter ("era"), the last known-good time checkpoint used to
restore an approximate system clock after the clock is lost, and a ring of
NTP-measured corrections that let queued timestamps (see
`bookorbit_stats.bin`) be resolved to real time retroactively.

Eras are keyed to clock continuity — a boot with a plausible system time
continues the previous era — because RTC memory does not reliably survive deep
sleep on this hardware. A clock loss therefore always opens a new era, which is
what lets a correction tell drift apart from powered-off time.

Each correction carries its sync anchor pair, which makes it a drift ramp rather
than a flat offset: `delta` is the error measured at `syncDeviceEpoch`, and
`windowStartEpoch` is the real time of the previous sync in the same era — an
instant where the error was zero by construction, since that sync set the clock.
Timestamps between the two are interpolated. `windowStartEpoch` is 0 when the era
opened on a clock loss, because the error then starts at the unknown powered-off
duration and `delta` applies as a flat shift instead.

Only eras where NTP actually ran appear in the ring; it replaces its lowest-era
entry when full, so timestamps survive several clock losses between syncs.

Binary layout (all little-endian):

- `[0-3]` magic + version: ASCII `WCK1`
- `[4-7]` `era` (`uint32_t`, incremented once per clock-loss boot)
- `[8-11]` `checkpointEpoch` (`uint32_t`, UTC epoch of the last trusted checkpoint)

Then `ERA_HISTORY` (6) correction records of 24 bytes each:

- `[0-3]` `era` (`uint32_t`)
- `[4]` `used` (`uint8_t`, 1 when the slot holds a valid record)
- `[5-7]` reserved
- `[8-15]` `delta` (`int64_t`, seconds of clock error at `syncDeviceEpoch`)
- `[16-19]` `windowStartEpoch` (`uint32_t`, real time of the previous same-era
  sync, or 0 for a flat correction)
- `[20-23]` `syncDeviceEpoch` (`uint32_t`, the clock reading just before the
  sync that measured `delta`)

## `section.bin`

### Version 62

Version 62 stores one compact source-whitespace bit per word in serialized text
blocks. Touch reader previews use it to reflow words with the selected font
without inferring spaces from device-specific pixel advances. Full and
suspended section caches rebuild together; complete files use version byte
`62`, and suspended partials use sentinel byte `0xF8`.

### Version 61

Version 61 is the v1.5.1 cache update. It stores `protectedImageUnits`
(`uint32_t` LE) after `pageCount`, so image-heavy sections estimate their
remaining non-image pages accurately. It also updates table fragments and
geometry, oversized-word wrapping, inline-image margins, and ruby continuation
layout. Full and suspended section caches rebuild together; complete files use
version byte `61`, and suspended partials use sentinel byte `0xF8`.

Each file in `sections/*.bin` stores one laid-out spine section. The header is
also the cache-busting key: if any layout-affecting setting differs from the
current reader settings, the section is discarded and rebuilt.

Version 59 adds a compact page-start visible-text-offset lookup table. The
offset is a Unicode codepoint coordinate in the spine XHTML, so reader progress
and KOReader sync can return to the same content after a font, orientation, or
indexing-method change instead of relying on a page percentage. Suspended
incremental caches store the same table for their readable prefix; a target
beyond that prefix must continue indexing before it can be resolved.

Version 57 is binary-identical to version 56. The version was bumped because
word-gap suppression now applies only to tokens glued together in the source.
Older caches could collapse explicit spaces between Hangul words, so full and
suspended partial section caches rebuild together. Version 58 recalculates
Bionic Reading split-run offsets with the renderer's combined advance and
kerning rounding, so old cached page positions rebuild.

Version 56 changes `<br>` layout: a line break after text no longer reapplies
the containing block's top or bottom spacing, while an empty `<br>` block keeps
the existing scene-break gap. Full and suspended partial section caches rebuild
together. Version 55 assigns compact IDs to internal EPUB links. The ID is
stored in the existing per-word flags byte and in each page's footnote entry so
touch devices can map tapped text to the existing fragment-navigation path
without retaining another per-word data structure. Version 54 adds compact
ruby-text annotations to serialized text blocks. Only words that begin a ruby
group store annotation text; continuation words use a dedicated style bit. This
keeps books without ruby markup unchanged apart from the cache version while
avoiding an empty string allocation for every word.
Version 53 stores each image's EPUB-internal source path so section indexing can
read only its header and defer full extraction until the page is shown. Version
52 keeps Guide Dots centered when extra word spacing is enabled. Version 51
preserves continuation state for oversized CJK word fragments. Version 50
paginates chapter-heading image runs within the reader viewport so they do not
overflow into the reserved status-bar area. Version 49 stores Bionic Reading
split-run offsets in visual order so RTL word prefixes render on the right.
Version 48 changed Arabic contextual shaping and text measurement, so cached
word positions from version 47 no longer match what `drawText` renders.

Version 48 makes the EPUB word-spacing level widen the natural inter-word gap
(each level adds 10 pixels), which changes laid-out word positions, so
older sections must rebuild. Version 46 added the EPUB word-spacing level to the
cache-busting header. It retains the flat `TextBlock` arena and chapter-opener
anchor behavior introduced in version 45. It includes:

- cache-busting fields for font, line compression, extra paragraph spacing,
  forced paragraph indents, paragraph alignment, viewport size, hyphenation,
  embedded CSS, image rendering mode, Bionic Reading, Guide Dots, word spacing,
  and EPUB render mode
- page offset LUT
- anchor-to-page map for fragment and footnote navigation
- paragraph and list-item LUTs used by KOReader sync page refinement
- visible-text-offset LUT used to resolve page positions across reflow and sync
- optional per-word Bionic Reading split metadata
- optional per-word Guide Dot x-offset metadata
- optional per-word text flags for CSS backgrounds, layout-inserted hyphens,
  and internal-link IDs
- reading-aid layout that stores Bionic Reading and Guide Dots as per-word metadata instead of temporary layout words
- publisher CSS page-break handling and adjusted justification spacing baked into page layout
- table fragments
- per-page footnote entries
- per-page publisher page markers
- serialized word style bits for underline, strikethrough, superscript, and
  subscript
- flat TextBlock word storage: per-word arrays plus one shared NUL-terminated
  text blob, replacing length-prefixed word strings and parallel vectors. The
  on-disk order mirrors the in-RAM arena so the firmware reads a whole block
  payload with a single allocation and a single SD read

ImHex pattern:

```c++
import std.mem;
import std.string;
import std.core;

#define EXPECTED_VERSION 61
#define MAX_STRING_LENGTH 65535
#define FOOTNOTE_NUMBER_LEN 32
#define FOOTNOTE_HREF_LEN 96

struct String {
    u32 length [[hidden, comment("String byte length")]];
    if (length > MAX_STRING_LENGTH) {
        std::warning(std::format("Unusually large string length: {} bytes", length));
    }
    char data[length] [[comment("UTF-8 string data")]];
} [[sealed, format("format_string"), comment("Length-prefixed UTF-8 string")]];

fn format_string(String s) {
    return s.data;
};

enum PageElementTag : u8 {
    TAG_PageLine = 1,
    TAG_PageImage = 2,
    TAG_PageTableFragment = 3,
    TAG_PageHorizontalRule = 4
};

enum WordStyle : u8 {
    REGULAR = 0,
    BOLD = 1,
    ITALIC = 2,
    BOLD_ITALIC = 3,
    UNDERLINE = 4,
    STRIKETHROUGH = 8,
    SUP = 16,
    SUB = 32
};

enum TextAlign : u8 {
    JUSTIFIED = 0,
    LEFT_ALIGN = 1,
    CENTER_ALIGN = 2,
    RIGHT_ALIGN = 3,
    NONE = 4
};

struct BlockStyle {
    TextAlign alignment;
    bool textAlignDefined;
    s16 marginTop;
    s16 marginBottom;
    s16 marginLeft;
    s16 marginRight;
    s16 paddingTop;
    s16 paddingBottom;
    s16 paddingLeft;
    s16 paddingRight;
    s16 textIndent;
    bool textIndentDefined;
    bool isRtl;
    bool directionDefined;
};

struct TextBlock {
    u16 wordCount;
    u8 hasBionic;
    u8 hasGuideDots;
    u8 hasWordFlags;
    u16 textBytes [[comment("Total size of text[], including one NUL per word")]];

    if (wordCount > 0) {
        u16 textOff[wordCount] [[comment("Byte offset of word i's text within text[]")]];
        s16 wordXPos[wordCount];
        if (hasBionic != 0) {
            u16 wordBionicSuffixX[wordCount] [[comment("Suffix x offset from word start")]];
        }
        if (hasGuideDots != 0) {
            u16 wordGuideDotXOffset[wordCount] [[comment("Guide dot x offset from word start; 0 means no dot")]];
        }
        WordStyle wordStyle[wordCount];
        if (hasBionic != 0) {
            u8 wordBionicBoundary[wordCount] [[comment("UTF-8 byte boundary between bold prefix and suffix")]];
        }
        if (hasWordFlags != 0) {
            u8 wordFlags[wordCount] [[comment("bit 0 = black background, bit 1 = layout-inserted trailing hyphen")]];
        }
        char text[textBytes] [[comment("All words back to back, each NUL-terminated")]];
    }

    BlockStyle blockStyle;
};

struct ImageBlock {
    String imagePath;
    String sourcePath;
    s16 width;
    s16 height;
};

struct PageLine {
    s16 xPos;
    s16 yPos;
    TextBlock block;
};

struct PageImage {
    s16 xPos;
    s16 yPos;
    ImageBlock image;
};

struct PageHorizontalRule {
    s16 xPos;
    s16 yPos;
    u16 width;
    u8 thickness;
};

struct TableFragmentCell {
    bool isHeader;
    u8 lineCount;
    TextBlock lines[lineCount];
};

struct TableFragmentRow {
    u16 height;
    bool headerSeparator;
    u8 cellCount;
    TableFragmentCell cells[cellCount];
};

struct PageTableFragment {
    s16 xPos;
    s16 yPos;
    u16 width;
    u8 columnCount;
    u8 cellPadding;
    u16 lineHeight;
    u8 rowCount;
    TableFragmentRow rows[rowCount];
};

struct PageElement {
    PageElementTag pageElementType;
    if (pageElementType == TAG_PageLine) {
        PageLine pageLine [[inline]];
    } else if (pageElementType == TAG_PageImage) {
        PageImage pageImage [[inline]];
    } else if (pageElementType == TAG_PageTableFragment) {
        PageTableFragment tableFragment [[inline]];
    } else if (pageElementType == TAG_PageHorizontalRule) {
        PageHorizontalRule horizontalRule [[inline]];
    } else {
        std::error(std::format("Unknown page element type: {}", pageElementType));
    }
};

struct FootnoteEntry {
    char number[FOOTNOTE_NUMBER_LEN];
    char href[FOOTNOTE_HREF_LEN];
    u8 linkId;
};

struct PublisherPageMarker {
    s16 yPos;
    char label[16];
};

struct Page {
    u16 elementCount;
    PageElement elements[elementCount] [[inline]];

    u16 footnoteCount;
    FootnoteEntry footnotes[footnoteCount];

    u8 publisherPageMarkerCount;
    PublisherPageMarker publisherPageMarkers[publisherPageMarkerCount];
};

struct AnchorEntry {
    String anchor;
    u16 page;
};

struct AnchorMap {
    u16 count;
    AnchorEntry entries[count];
};

struct ParagraphLut {
    u16 count;
    u16 paragraphIndex[count];
};

struct SectionBin {
    u8 version;
    if (version != EXPECTED_VERSION) {
        std::error(std::format("Unsupported version: {} (expected {})", version, EXPECTED_VERSION));
    }

    s32 fontId;
    float lineCompression;
    bool extraParagraphSpacing;
    bool forceParagraphIndents;
    u8 paragraphAlignment;
    u16 viewportWidth;
    u16 viewportHeight;
    bool hyphenationEnabled;
    bool embeddedStyle;
    u8 imageRendering;
    bool bionicReadingEnabled;
    bool guideReadingEnabled;
    u8 wordSpacing;
    u8 renderMode; // 0 = CrossInk Default, 1 = Balanced, 2 = Light

    u16 pageCount;
    u32 protectedImageUnits;
    u32 pageLutOffset;
    u32 anchorMapOffset;
    u32 paragraphLutOffset;
    u32 listItemLutOffset;

    Page pages[pageCount];

    u32 currentOffset = $;
    if (currentOffset != pageLutOffset) {
        std::warning(std::format("Page LUT offset mismatch: expected 0x{:X}, got 0x{:X}", pageLutOffset, currentOffset));
    }

    u32 pageLut[pageCount] [[comment("Page data offsets")]];

    if (anchorMapOffset != 0) {
        AnchorMap anchorMap @ anchorMapOffset;
    }

    if (paragraphLutOffset != 0) {
        ParagraphLut paragraphLut @ paragraphLutOffset;
    }

    if (listItemLutOffset != 0 && paragraphLutOffset != 0) {
        u16 listItemIndex[paragraphLut.count] @ listItemLutOffset;
    }
};

SectionBin section @ 0x00;

u32 fileSize = std::mem::size();
u32 parsedSize = $;
if (parsedSize != fileSize) {
    std::warning(std::format("Unparsed data detected: {} bytes remaining at offset 0x{:X}", fileSize - parsedSize, parsedSize));
}
```
