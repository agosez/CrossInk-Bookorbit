# File Formats

These formats describe the SD-card cache files under `/.crosspoint/epub_<hash>/`.
All POD fields are written in the ESP32 little-endian representation used by
`Serialization.h`; strings are length-prefixed UTF-8 unless a format notes a
fixed-size char buffer.

## `epub_<hash>/links.bin`

The EPUB reader writes followed-link Back history on clean exit (Home, sleep,
or reader replacement for sync). The record has a one-byte depth (1–3), followed
by that many pairs of little-endian `u16` spine index and `u16` page number,
oldest first: 5, 9, or 13 bytes. Empty history removes the file. A transient
footnote preview resumes at its immediate origin and omits that final entry;
earlier full-section links remain in the record.

On open, the reader checks the exact length and each spine index, closes the
file, and deletes it before adopting the history. Malformed records are also
consumed. A later clean exit rewrites the current stack; an unclean shutdown
cannot revive history from a previous session. This new sidecar does not change
EPUB layout cache formats. As with in-memory Back history, changing font or
layout may shift the destination page.

## `/.crosspoint/home_carousel_cache_<index>.bin`

### Version 6

The v1.6.1 release normalizes development version 8 to version 6, one step
after v1.6.0. The new per-position filenames and artwork cache keys prevent
reuse of older combined snapshots.

Each Carousel position has a disposable snapshot containing only cover artwork,
titles and position dots. Progress, reading time, header, menu icons and button
hints are drawn live after restoration. Each file contains a `CarouselCacheHeader`
followed by one full framebuffer. The header's `frameCount` records the number of
recent books used to compose the artwork, rather than the number of stored frames.
The key tracks ordered book paths, titles, cover paths, thumbnail availability and
image polarity, rather than reading progress or statistics.

Frames are rendered and saved only when viewed; returning Home does not prepare
other positions in advance. The development version 7 combined
`home_carousel_cache.bin` is removed after the first successful write. Cache
regeneration is automatic; EPUB layout caches and reading history are unaffected.

## `/.crosspoint/ttf-rendering.json`

This user-owned JSON file stores only custom TTF families whose raster settings
differ from CrossInk's defaults. Each entry is keyed by the installed family name
and contains numeric hinting, raster, interpreter, weight, and slant choices plus
the stem-darkening toggle. Missing families use automatic hinting, grayscale
output, the default interpreter, and no outline adjustments. The file currently
keeps at most 24 modified family profiles to bound RAM use while settings are open.

## `/.crosspoint/sleep-image-index/<directory-hash>-{bmp,all}.idx`

### Version 1

`ImageFolderIndex` (`src/activities/boot_sleep/ImageFolderIndex.{h,cpp}`) keeps
a compact, rebuildable index per indexed folder. It backs both the sleep-image
folder and the boot-screen folder (`/bootscreen` or `/.bootscreen`); the
directory name predates the boot-screen use and was kept as-is so existing
sleep caches aren't orphaned by an unrelated rename. The index avoids walking
a folder on every selection while using only one fixed-size record at a time
in RAM. `bmp` contains BMP files and `all` contains BMP and PNG files (sleep's
Page Overlay mode only; the boot screen is BMP-only and always uses a `bmp`
index). The `validated` header flag means BMP headers were checked while
rebuilding after a failed render.

The index is disposable: a missing, malformed, or stale selected entry causes
one rebuild and then the caller falls back to its directory scan. File
transfer, file-browser, and preferred-folder changes invalidate affected
indexes via `ImageFolderIndex::invalidateForPath()`. Files added or changed
directly on the SD card have no notification path; they are picked up when a
cached entry is found missing or when an index is otherwise rebuilt.

```c++
struct ImageFolderIndexHeader {
    char magic[4];       // "CSIX"
    u8 version;          // 1
    u8 flags;            // bit 0: BMP+PNG, bit 1: BMP headers validated
    u16 pathLength;
    u16 recordCount;
    u16 recordSize;      // sizeof(ImageFolderIndexRecord)
    u32 recordsOffset;   // sizeof(header) + pathLength
    char directory[pathLength];
};

struct ImageFolderIndexRecord {
    u16 nameLength;
    u8 flags;             // bit 0: PNG (otherwise BMP)
    u8 reserved;
    char name[256];      // zero-padded UTF-8 filename, max 255 bytes
};
```

## `/.crosspoint/library.idx`

### Version 6

Each book's name blob now ends with a `uint32_t` series position after the
length-prefixed genre. The value is a sortable encoding of a finite signed
IEEE-754 single-precision number; `0xFFFFFFFF` means missing or invalid. Series
sorting compares folded series name, then this position, then title order. Missing
positions follow numbered books within a series. Calibre `series_index` and
EPUB 3 `group-position` supply the value. Version 5 and earlier indexes rebuild
on the next Library scan; EPUB metadata is reread when enabled to obtain the new
field, while `firstSeen` history survives reconciliation.

### Version 5

Date Added now uses the filesystem creation timestamp. A title-ordered array of
`uint32_t` packed FAT creation date/times follows the five `uint16_t` sort
permutations and precedes the aligned name section. A zero value means the
creation timestamp is unavailable; `firstSeen` orders books with equal or
missing timestamps, and zero sorts before dated values in ascending order. The
128-byte book record still stores modification time
separately for EPUB metadata freshness. Version 4 indexes rebuild on the next
Library scan, retaining metadata and `firstSeen` values from the old index.
Header flag bit 2 marks an arrival order that fell back to `firstSeen` because
sorting by creation time ran out of memory; the next scan retries it. Version 4
uses only bits 0 and 1, so its flags remain valid during reconciliation.

### Version 4

Two more `uint16_t` permutations follow arrival order: series order and genre
order. Both use folded EPUB metadata, place missing values last in ascending
order, and use title order to break ties. The header, records, and name blobs
remain compatible with version 3. During the one-time rebuild, version 3
metadata and `firstSeen` arrival history are reused for unchanged books.

### Version 3

The Library index adds two length-prefixed fields after source author in each
book's name blob: series and genre. The fixed-size header and record layout stay
the same. A version 2 index is read once during rebuilding so existing
`firstSeen` values survive; the new index is then written as version 3.
Series and genre are read only from EPUB metadata. The library treats the first
`dc:subject` value as genre, and reads Calibre or EPUB 3 series metadata.

### Version 2

Version 2 added EPUB title and author metadata, the file modification time,
and source-author spelling to the original Library index. Its header and
128-byte records are compatible with version 3 reconciliation; its name blobs
do not contain series or genre.

### Version 1

`LibraryIndexFile` (`lib/LibraryIndex/LibraryIndexFile.{h,cpp}`) reads the
`CLX1` on-disk index for the Library screen: one sorted, searchable
snapshot of up to 32,767 books on the card, built by `LibraryBuilder` so paging,
sorting, and searching the shelf cost a handful of seeks instead of a
directory walk per screen. The format itself (`lib/LibraryIndex/LibraryFormat.h`)
is free of `HalStorage` and Arduino so its layout and validation rules are
host-testable (`test/library_format`, `test/library_index_file`).
Builds keep RAM flat on every device: each sort holds a fixed buffer and spills
sorted runs to the card when a library outgrows it, and the previous index is
matched through a sorted file rather than an in-RAM table. While building, the
transient files `library.stage`, `library.stage.f`, `library.prior`,
`library.rename`, `library.order`, `library.authors`, `library.canon`, and
`library.runs` live in `/.crosspoint`; every build removes them when it ends. If
the scan finds another book beyond the limit, the rebuild fails and keeps the
previous index instead of publishing a partial shelf.

Every section starts on a 512-byte boundary. Records are a fixed 128 bytes
each, so record `k` always lives at `recordStart + 128*k` with no offset table
to load first, and 32 records exactly fill a 4096-byte scan buffer. Five
`uint16_t` permutation arrays (surname, first name, arrival, series, then genre
order) and one `uint32_t` creation-time array let those sorts page without
re-sorting on every open;
Title order needs no permutation because the record section is already
title-sorted.

The index is disposable: a bad magic, an unknown format/fold version, a size
mismatch (a build interrupted by power loss cannot pass, since `selfSize` is
checked against the real file size), or an inconsistent section layout all
cause a full rebuild rather than a crash or silently wrong output. A fold
version bump alone (the text-normalisation rules changed) can be handled by
reconciliation instead: `openForReconciliation()` accepts stale sort/search
keys so each book's `firstSeen` arrival order survives across the rebuild
even though its fold and permutations are regenerated.

CrossInk's format version is `6`; older indexes rebuild automatically. Versions
2 through 5 can be read for reconciliation so arrival history survives. The fold
version is `3`.

```c++
struct ClixHeader {            // 64 bytes, padded to the first 512-byte sector
    char magic[4];              // "CLX1"
    u8 formatVersion;           // 6
    u8 foldVersion;             // 3
    u8 flags;                   // bit0: ranks degraded, bit1: dedup degraded, bit2: arrival degraded
    u8 metadataEnabled;         // 0 or 1
    u16 bookCount;
    u16 folderCount;
    u16 nextFirstSeen;
    u16 padding1;
    u32 folderStart;
    u32 folderLen;
    u32 recordStart;
    u32 permStart;
    u32 nameStart;
    u32 nameLen;
    u32 selfSize;                // expected total file size; truncation guard
    u8 reserved[20];
};

struct ClixRecord {             // exactly 128 bytes; record k @ recordStart + 128*k
    u32 nameOff;                 // offset into the name blob, from nameStart
    u32 fileSize;                // captured while the dirent was open
    u16 firstSeen;
    u16 folderId;                // index into the folder table
    u8 nameLen;
    u8 foldLen;
    u8 authorKeyLen;
    u8 metadataStatus;           // 0 not attempted, 1 extracted, 2 failed
    char fold[96];               // folded sort/search key
    char authorKey[12];          // order-insensitive author identity
    u32 modificationTime;        // packed FAT date/time, or 0 if unavailable
};

struct ClixFolderHeader {        // one per indexed folder, back to back
    u8 pathLen;                  // 1..255; path bytes follow, no trailing '/'
};
```

The name blob for each record (found via `nameOff` into the `names` section)
holds, back to back: an 8-byte FNV-1a path hash of the book's complete path
(the identity used by rebuild reconciliation and by "is this book already in
the index" lookups), the filename, then five length-prefixed fields —
display author, title, the pre-spelling-harmonisation source author, series,
and genre. Version 6 appends the four-byte series position.

## `/.crosspoint/library.meta` and `/.crosspoint/library.metd`

### Version 1

`LibraryMetadataCache` (`lib/LibraryIndex/LibraryMetadataCache.{h,cpp}`) keeps
EPUB metadata from the moment each book is parsed, independent of whether the
Library build that parsed it finishes. A later build looks a book up only when
the previous `library.idx` cannot supply reusable metadata, so a cancelled or
failed scan resumes without re-parsing what it already read. Failed parses are
not stored. A book is identified by its complete-path FNV-1a hash (the same
`clixPathHash` the index uses), file size, and packed FAT modification time;
books with no modification time are never cached.

`library.meta` holds a 32-byte header padded to 512 bytes, then 65,536 16-byte
slots of an open-addressing hash table (linear probing, at most 64 probes). An
all-zero slot is empty. A slot whose check does not match is skipped. There is
one slot per path; storing a changed book replaces its slot's payload offset.

`library.metd` is append-only. Each record is a 32-byte header followed by
title, author, series, and genre bytes (each at most 255 bytes, cut at a UTF-8
boundary). Payloads are written before the slot that points at them, and every
read verifies the record checksum, so a torn write costs a re-parse rather than
wrong metadata. The cache is discarded and recreated when the header does not
validate, more than 75% of slots are used, or the payload passes 64 MiB. Bump
the cache version whenever the set of extracted metadata fields changes.

```c++
struct CacheHeader {             // 32 bytes at offset 0
    char magic[4];               // "CLM1"
    u8 version;                  // 1
    u8 padding[3];
    u32 slotCount;               // 65536
    u32 usedSlots;               // slots ever claimed; written on close
    u8 reserved[16];
};

struct CacheSlot {               // slot i @ 512 + 16*i
    u64 pathHash;
    u32 payloadOffset;           // into library.metd
    u32 check;                   // FNV-1a of pathHash and payloadOffset, low bit set
};

struct PayloadHeader {           // 32 bytes, then the four strings back to back
    u32 magic;                   // "CLMP"
    u64 pathHash;
    u32 fileSize;
    u32 modificationTime;
    u32 seriesPosition;          // same encoding as library.idx version 6
    u8 titleLen;
    u8 authorLen;
    u8 seriesLen;
    u8 genreLen;
    u32 checksum;                // FNV-1a of this header (checksum zeroed) and the strings
};
```

## `book.bin`

### Version 10

`book.bin` stores EPUB metadata plus lookup tables for spine and TOC entries.
The current firmware writes this version from `BookMetadataCache`.
Version 10 rebuilds metadata with namespace-aware OPF parsing so optimizer-generated
XML prefixes do not leave an empty chapter list. The binary layout is unchanged.
Version 9 stores book and TOC title strings NFC-composed so decomposed
diacritics render correctly with device fonts. It also rebuilds metadata after
the EPUB guide start-reference handling changed.

ImHex pattern:

```c++
import std.mem;
import std.string;
import std.core;

#define EXPECTED_VERSION 10
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

### Version 10

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
display setting. Version 10 appends a field mask so a book overrides only the
reader settings that differ from its current global defaults. Version 2-9
records with the custom-settings flag keep their full snapshot as an override
when migrated; they cannot distinguish past manual edits from automatic ones.
The file can preserve an auto-page-turn interval without forcing custom
font/layout settings for the book. It also stores a per-book EPUB render mode override,
which can be changed from book action menus before opening the book so a
problematic EPUB can be moved to Balanced or Light rendering without entering
the reader first. Safe Mode also uses this file to save Light rendering with
embedded styles, Focus Reading, and Guide Dots disabled after that final
fallback successfully opens a difficult book.

```c++
struct ReaderSettingsBin {
    u8 version; // 10
    u8 flags;   // bit 0 = at least one custom reader field, bit 1 = custom auto-page-turn interval, bit 2 = render mode override, bit 3 = dictionary font override, bit 4 = Safe Mode override
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
    u8 focusReadingEnabled;
    u8 guideReadingEnabled;
    u8 snapshotRenderMode;
    u8 indexingMethod; // 0 = incremental, 1 = full section
    char sdFontFamilyName[64];
    char dictionarySdFontFamilyName[64]; // meaningful only when flag bit 3 is set
    u8 dictionaryFontPointSize; // 0 = follow reader size
    u32 readerSettingsOverrideMask; // bits 0-17 correspond to snapshot fields above, excluding snapshotRenderMode; bit 18 = sdFontFamilyName
};
```

## `/.crosspoint/clippings/<bookType>_<contentHash>.bin`

### Versions 1-4

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

- `[0]` version (`1`, `2`, `3`, or current version `4`)
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
  - versions 3-4: reader layout signature (`uint32_t` LE; font, spacing,
    viewport, and other section-layout inputs)
  - version 4: table selection (`uint16_t` LE; `UINT16_MAX` for non-table text)
  - `chapterTitle` (`char[48]`, null-terminated/truncated)
  - version 1: selected text (`String`; legacy files were written with a
    `512`-byte in-app limit)
  - versions 2-4: selected-text length (`uint16_t` LE) followed by that many
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
same total page count. Legacy records without a layout signature use text
matching rather than trusting ambiguous numeric ranges. Version 4 adds the
table selection field; versions 1-3 remain readable.

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

The watermark also records whether the file has been through an exchange: it stays `0`
until the first exchange that goes through, which sets it to `1` when no upload moved it
further. `1` is 1970-01-01 00:00:01 and hides no highlight. A sync reports the key set of
a file that has been through an exchange as BookOrbit's `known` sidecar state, which
BookOrbit 3.5.0 needs to honour the deletion of every synced highlight of a book instead
of reading it as a lost copy. The mark lives in this file so that it is lost exactly
when the records are: a file started over by a new highlight is not `known` until it has
been through an exchange itself. Highlights received from the server are never uploaded
(their clippings have no word range), so the watermark only ever follows highlights made
on the device.

- `[0-3]` magic + version: ASCII `BOA1`
- `[4-7]` `watermark` (`uint32_t`, newest highlight timestamp the server has accepted; `1` after an exchange that sent nothing; `0` until the first exchange)
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

The watermark marks a file that has been through an exchange the same way as in
`bookorbit_annotations.bin`.

- `[0-3]` magic + version: ASCII `BOB1`
- `[4-7]` `watermark` (`uint32_t`, newest identityEpoch the server has accepted; `1` after an exchange that sent nothing; `0` until the first exchange)
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

## `bookorbit_sync.bin`

`bookorbit_sync.bin` is BookOrbit Smart sync's memory of a book, in the book's
content-keyed state directory (`/.crosspoint/book_<contentHash>/`): what this device saw
at the end of its last successful sync of the book. Every sync that settles the progress
rewrites it (an upload, an applied server position, or both sides found at the same
place), whichever Sync Behavior is selected. A sync that leaves the choice to the user,
or that the sleep sync leaves for a manual sync, does not touch it.

The file carries no version field: the two layouts share their magic and differ by
length, the second being the first with a position appended. A release that only knows
the first layout reads its 12 bytes of a longer file and ignores the rest.

### Version 2

Adds the position the sync left both sides at, in local pages. It is what tells a server
position that only changed its date (BookOrbit's web reader saves the position a book
opens at) from one that moved: the next sync counts the server as not having moved when
its position, mapped into local pages, is in this spine item at or before this page, one
page of tolerance included.
`pageNumber` is a fraction of `pageCount`, so it still means the same place after the
chapter is laid out again with another page count.

Binary layout (all little-endian), 18 bytes:

- `[0-3]` magic (`uint32_t`, `0x424F5359`, ASCII `YSOB` on disk)
- `[4-11]` `timestamp` (`int64_t`, the server's progress timestamp in UTC epoch seconds:
  the one it reported for an applied or matching position, the one this device sent
  with an upload)
- `[12-13]` `spineIndex` (`uint16_t`)
- `[14-15]` `pageNumber` (`uint16_t`, zero-based page in that spine item)
- `[16-17]` `pageCount` (`uint16_t`, page count of that spine item when the position was
  recorded; `0` means no position, and the file reads as version 1)

### Version 1

Written up to v1.6.1+bookorbit.1: the first 12 bytes of version 2, the date alone. Still
read; until the book's next settled sync rewrites it, the server counts as having moved
whenever its timestamp is newer.

A file shorter than 12 bytes, with another magic or with a timestamp of 0 or less, is no
history at all: Smart sync then asks, and the sleep sync leaves the progress alone.

## `/.crosspoint/bookorbit_downloads.bin`

### Version 1

Remembers where BookOrbit catalog downloads landed on the SD card, keyed by the
server's book id, so the catalog can mark a book as already on the device even
when the download folder setting has since changed or the file was renamed
through a supported flow. It is also what the catalog's offline "On device"
category lists, which is how a download stays reachable however deeply the
server's file naming template filed it, without walking the card. It is a
best-effort convenience cache: entries are verified on lookup (the file must
still exist with its recorded size) and dropped when stale, and a missing or
discarded index costs the marker and the listing entry, leaving the filename
heuristic and a flat directory scan as fallbacks. Capped at 128 entries, oldest
evicted first.

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

### Version 83

Nested paragraphs and other blocks retain inherited CSS bold and italic styles,
including explicit child overrides. The payload is unchanged from version 82,
but glyph styles and wrapping can differ. Complete files use byte `83`;
suspended partials use `0xC4`. Older full and partial caches rebuild automatically
so previously cached regular text does not hide the corrected styling. The CSS
rule cache format is unchanged.

### Version 82

Scalable-font EPUB headings and whole text blocks carry a resolved point size
and line height. Each serialized `TextBlock` appends `u8 fontSize` (0 = reader
font, otherwise 8-44 pt) and `u16 lineHeight` after `directionDefined` in its
`BlockStyle` payload. The inherited Q8 font scale is layout-only and is not
serialized. Complete section files use byte `82`; suspended partials use `0xC3`.
Older full and partial caches rebuild automatically.

CSS cache revision `20` adds a five-byte font-size length (float value plus unit)
after `imageWidth` and uses defined-property bit 23. The fixed style payload is
76 bytes. Older CSS caches rebuild automatically.

On scalable fonts, headings default to 2, 1.5, 1.17, 1, 0.83, and 0.67 times the
inherited size, rounded to whole points and bounded to 8-44 pt. Enabled book
styles can override block sizes using em, rem, %, px, pt, and size keywords.
CSS 16px/12pt maps to the user's selected body size; em/% use the parent and
rem uses the HTML root. Inline span size changes and table-cell sizing remain
uniform in this phase. Light mode and bitmap fonts retain their existing sizes.

### Version 81

Version 81 carries `text-indent` from the HTML and body root styles into
descendant paragraph blocks. Existing full section caches (byte `80`) and
suspended partial caches (`0xC1`) rebuild so inherited paragraph indentation is
reflected in saved page positions. Complete files use byte `81`; suspended
partials use marker `0xC2`. The CSS rule cache format is unchanged.

### Version 80

Version 80 places small inline images within text lines while keeping larger
images as centered blocks. Page-image records add a one-byte inline flag after
their coordinates; older full and suspended partial section caches rebuild so
existing books receive the new layout. The CSS rule cache moves to version `19`
so `display: inline` rules retain their meaning.

The v1.6.1 release candidate used version `80` and partial marker `0xC1`: development
versions `78` and `79` used the older image payload, and earlier release
preparation used partial marker `0x80`. Reusing those identifiers could accept
incompatible saved pages. Version `81` and marker `0xC2` added the root-style
indentation change; the final v1.6.1 release uses version `83` and marker `0xC4`
for the further layout changes described above. All of those older caches rebuild
automatically.

### Version 79

Version 79 keeps the version 78 serialized layout. Korean words now wrap at
spaces by default; with hyphenation enabled, they can also split at a legal
CJK boundary at a line end without a visible hyphen. Justification stretches
word spaces only. Full caches (byte `79`) and suspended partial caches
(`0xF4`) both rebuild because earlier page positions are no longer valid.

### Version 78

Version 78 changed layout for inline CSS padding. Full and suspended partial
section caches rebuild together.

### Version 77

Version 77 keeps the serialized layout unchanged. It was bumped because ordered
lists now number their items, `list-style-type: none` suppresses markers, and
`<ul>`/`<ol>` margins and padding contribute to child insets. Complete files use
byte `77`; suspended partials use the previously unused sentinel `0xF3`.
The related CSS rule cache uses version `18`; version `17` already occurs in
local branch history.

### Version 75

Version 75 keeps the serialized layout unchanged but excludes EPUB elements with
the HTML `hidden` attribute. Complete files use byte `75`; suspended partials use
`0xF4`. Both older full and partial layouts rebuild automatically.
Versions 67–74 and partial sentinel 0xF5 already occur in other local branch
history; using fresh identifiers avoids accepting those experimental caches.

### Version 66

Version 66 keeps the version 63 serialized layout unchanged. It was bumped
because internal EPUB links now preserve CSS superscript and subscript styles,
changing their cached word-style flags and page layout. Complete files use
version byte `66`, and suspended partials use sentinel byte `0xF6`.

The stable v1.5.1 release retains these identifiers from RC6. Do not normalize
published RC versions to the previous stable version plus one: v1.5.0 used
`60` / `0xF9`, and RC4 already shipped `61` / `0xF8` with older layout output.
Reusing those identifiers could accept stale RC caches as current. Version 9
per-book reader settings and their version 7/8 migrations remain
readable. Version 10 adds a field-override mask so a book can inherit unrelated
global reader settings.

### Version 62

Version 62 stores one compact source-whitespace bit per word in serialized text
blocks. Touch reader previews use it to reflow words with the selected font
without inferring spaces from device-specific pixel advances. Full and
suspended section caches rebuild together; complete files use version byte
`62`, and suspended partials use sentinel byte `0xF8`.

### Version 61

Version 61 was an earlier v1.5.1 release-candidate cache update. It stores `protectedImageUnits`
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
Focus Reading split-run offsets with the renderer's combined advance and
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
overflow into the reserved status-bar area. Version 49 stores Focus Reading
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
  embedded CSS, image rendering mode, Focus Reading, Guide Dots, word spacing,
  and EPUB render mode
- page offset LUT
- anchor-to-page map for fragment and footnote navigation
- paragraph and list-item LUTs used by KOReader sync page refinement
- visible-text-offset LUT used to resolve page positions across reflow and sync
- optional per-word Focus Reading split metadata
- optional per-word Guide Dot x-offset metadata
- optional per-word text flags for CSS backgrounds, layout-inserted hyphens,
  and internal-link IDs
- reading-aid layout that stores Focus Reading and Guide Dots as per-word metadata instead of temporary layout words
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

#define EXPECTED_VERSION 79
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
    u8 fontSize;
    u16 lineHeight;
};

struct TextBlock {
    u16 wordCount;
    u8 hasFocus;
    u8 hasGuideDots;
    u8 hasWordFlags;
    u16 textBytes [[comment("Total size of text[], including one NUL per word")]];

    if (wordCount > 0) {
        u16 textOff[wordCount] [[comment("Byte offset of word i's text within text[]")]];
        s16 wordXPos[wordCount];
        if (hasFocus != 0) {
            u16 wordFocusSuffixX[wordCount] [[comment("Suffix x offset from word start")]];
        }
        if (hasGuideDots != 0) {
            u16 wordGuideDotXOffset[wordCount] [[comment("Guide dot x offset from word start; 0 means no dot")]];
        }
        WordStyle wordStyle[wordCount];
        if (hasFocus != 0) {
            u8 wordFocusBoundary[wordCount] [[comment("UTF-8 byte boundary between bold prefix and suffix")]];
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
    bool focusReadingEnabled;
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

## Optimizer image transport (PXC2 and COIX)

Optimizer images are optional EPUB sidecars. Original JPEG/PNG assets remain the
compatibility fallback. `META-INF/crossink/optimizer-v1.json`,
`META-INF/crossink/optimizer-images-v1.idx`, and all
`META-INF/crossink/pxc/*.pxc2` entries must use ZIP STORE. PXC2 has its own block
compression; the firmware rejects ZIP-deflated PXC2 before starting an inflater.
The manifest keeps `format: "crossink-optimizer"` and `version: 1`. Each image has
`href`, `pxc`, `width`, `height`, `pxcFormat: "pxc2"`, `pxcBytes` (complete transport
size), and `pixelCrc32`. Missing `pxcFormat` means legacy raw PXC1.

All integers below are unsigned little endian. Fields are serialized explicitly,
not by writing native C++ structures. CRCs use standard IEEE CRC32 (zlib).

### PXC2

The 32-byte header is:

| Offset | Bytes | Field                                                              |
| ------ | ----- | ------------------------------------------------------------------ |
| 0      | 4     | `PXC2`                                                             |
| 4      | 1     | Version 2                                                          |
| 5      | 1     | Pixel format 1: four 2-bit pixels per byte, most significant first |
| 6      | 2     | Header bytes: 32                                                   |
| 8      | 2     | Width                                                              |
| 10     | 2     | Height                                                             |
| 12     | 2     | Row bytes: `(width + 3) / 4`                                       |
| 14     | 2     | Block bytes: 2048                                                  |
| 16     | 2     | Block count: ceiling of raw bytes / 2048                           |
| 18     | 2     | Flags: zero                                                        |
| 20     | 4     | Raw pixel bytes                                                    |
| 24     | 4     | Raw pixel CRC32                                                    |
| 28     | 4     | Complete PXC2 file bytes                                           |

Dimensions are 1–1024, raw data is at most 128 KiB, and there are at most 64
blocks. Each block starts with a 12-byte header: codec (`u8`, 0 RAW, 1 raw DEFLATE,
2 PackBits), zero flags (`u8`), raw length (`u16`), encoded length (`u16`), sequence
(`u16`, starting at zero), decoded-block CRC32 (`u32`). Lengths are 1–2048. All
non-final blocks have 2048 raw bytes. Compressed blocks must be smaller than raw;
otherwise producers use RAW. Every block is independent. PackBits controls 0–127
copy the next control+1 bytes; 129–255 repeat the next byte 257-control times;
128 is rejected. Raw DEFLATE must terminate exactly, with no extra input/output.
Python uses zlib level 8 and `wbits=-15`; the browser uses PackBits/RAW.

The reader reuses a fallibly allocated session workspace (5928 bytes on the
64-bit host; firmware size is compile-time limited below 6500), verifies all
lengths/CRCs and resizes rows into `img_*.pxc.optimizer.tmp`. It checks size and
syncs/closes before publishing. On filesystems that cannot rename over an existing
file, `.optimizer.previous` retains the previous cache until publication succeeds;
failed rollback leaves that backup available for recovery on the next attempt. PXC2 never needs a full raw source temporary file.
The local raw PXC layout remains two `u16` dimensions plus packed rows. No section
cache version change is required. Hardware heap and visual results remain device
acceptance checks, not implied by host workspace accounting.

### COIX version 1

The local index is `/.crosspoint/epub_<hash>/optimizer-images.idx`. Its header is:

| Offset | Bytes | Field                            |
| ------ | ----- | -------------------------------- |
| 0      | 4     | `COIX`                           |
| 4      | 2     | Version 1                        |
| 6      | 2     | Header bytes: 32                 |
| 8      | 2     | Record bytes: 208                |
| 10     | 2     | Record count: 0–256              |
| 12     | 4     | Flags/reserved: zero             |
| 16     | 4     | Manifest central-directory CRC32 |
| 20     | 4     | Manifest uncompressed bytes      |
| 24     | 4     | CRC32 of all records             |
| 28     | 4     | CRC32 of header bytes 0–27       |

Each 208-byte record contains NUL-terminated `href[129]` (offset 0),
`pxcHref[65]` (129), width `u16` (194), height `u16` (196), format `u8` (198,
1 legacy or 2 PXC2), zero flags `u8` (199), complete sidecar bytes `u32` (200),
and pixel CRC32 `u32` (204). Paths are relative, bounded UTF-8; traversal,
backslashes, control characters, colons and percent escapes are rejected.
Sidecars must be beneath `META-INF/crossink/pxc/`. Duplicate image hrefs are invalid;
producers keep one optional sidecar per href and omit unsupported/over-limit
entries. Firmware can resize that sidecar for other layouts.

Reader setup validates a packaged index and copies it through a temporary local
file, or builds it from a legacy manifest while the framebuffer is loaned. Generic
`Epub::load()` does not build indexes. A lookup scans records on SD and retains
only one last hit. Manifest identity changes invalidate the local index. Per-page
preflight reuses local pixels first, then materializes just that page's sidecars,
then extracts original assets as fallback. Legacy ZIP inflation also runs under
the framebuffer loan. Sleep-page generation uses the same preflight. Temporary
index files are cleaned during setup and per-image temporaries during preflight.

Hardware acceptance: clear only the test book's cache, open legacy/PXC2 variants
with SD font and AA, visit/revisit image pages and sleep, compare portrait and
landscape output, and record internal free/largest heap blocks and low-water
marks. Repeat corrupt sidecars, full/read-only SD, interrupted writes and book
replacement at the same path on X3/X4, Sticky (SPI SD) and X4 Pro (SDMMC).

### CSS rules cache revision 18

Revision 18 adds the serialized `list-style-type` property used to number
ordered lists and suppress list markers. It also includes the PSRAM streamed
stylesheet path introduced in revision 16, which admits sources up to 512 KiB
on PSRAM readers while C3 retains its 128 KiB limit. Existing rule-count and
internal-memory guards still apply. Rebuilding an invalid CSS cache also
invalidates section caches through the existing EPUB-load path, so books that
previously cached zero rules can restore hidden content and layout rules.

## S3 scalable reader fonts

Static TTF support uses font-content and backend identities to invalidate
affected EPUB layouts. Section-cache serialization is unchanged. Existing
`.cpfont` files remain supported; see [scalable fonts](scalable-fonts.md) for
limits and lifecycle.

## `/.crosspoint/font-catalog.bin`

### Version 1

Disposable font metadata cache, shared by reader, settings and web font controls.
The 24-byte little-endian header contains magic `0x46434931`, version, a 64-bit
inventory fingerprint, family count (maximum 128), and scalable-font build mode.
It is followed by 152-byte family summaries: a NUL-terminated 128-byte name,
32-bit detail offset/byte count/FNV-1a hash, 16-bit file count, minimum/maximum
point sizes, a scalable flag, three reserved zero bytes, and a 32-bit FNV-1a
checksum of the preceding summary bytes. Detail blocks follow the summaries.
Each detail is three bytes (point size, style, path length) followed by the
UTF-8 path bytes. Paths are at most 255 bytes; families contain at most 256
files. The whole cache is capped at 2 MiB.

Names/range labels load without retaining file paths. Only a requested family
hydrates its detail vector. Missing, incompatible or malformed summaries rebuild
the catalog by scanning names first and writing one family's paths at a time;
invalid detail blocks invalidate the cache for the next request.
Temporary memory failures preserve the font selection and cache for retry.
Writers finish and sync `font-catalog.tmp` before replacing the cache. An
interrupted replacement is safe because no user data is stored here.

On first font metadata access after boot or explicit invalidation, an inventory
walk checks names, directory kinds and file lengths in both font roots and their
immediate subdirectories. It reads no font contents on an index hit, and is not
repeated when catalog RAM is released and reloaded during the same session. This
detects externally added/removed files and changed lengths after restarting;
same-length content-only edits outside the firmware are not detectable by that
check. Firmware upload/delete/move/download paths invalidate explicitly,
including failed dedicated font uploads; USB Drive invalidates before handing
the card to the host. Manage Fonts performs a full rescan; alternatively remove
this cache to force reinspection after external same-length font changes.

EPUB layout cache versions and identities are unchanged by this catalog.
