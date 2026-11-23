"""On-card formats the scenarios write and read back.

The reader and BookOrbit stores mirror the firmware's own serialization (the source file
named at each one). The download index follows docs/file-formats.md instead of the
firmware, so the scenarios also hold that document to account.
"""

from __future__ import annotations

import base64
import json
import struct
import zlib
from pathlib import Path


def fnv1a32(data: bytes) -> int:
    h = 2166136261
    for b in data:
        h = ((h ^ b) * 16777619) & 0xFFFFFFFF
    return h


def obfuscate(plaintext: str, mac: bytes) -> str:
    """obfuscation::obfuscateToBase64 for one device: "CPV1" + FNV1a32(MAC || plaintext) LE
    + plaintext, XOR MAC, base64. The MAC is the ESP32 base MAC, the Wi-Fi station one."""
    payload = b"CPV1" + struct.pack("<I", fnv1a32(mac + plaintext.encode())) + plaintext.encode()
    return base64.b64encode(bytes(c ^ mac[i % len(mac)] for i, c in enumerate(payload))).decode()


# --- reading progress (EpubReaderUtils.h) ---------------------------------------------


def write_progress_bin(path: Path, spine: int, page: int, page_count: int,
                       visible_text_offset: int | None = None) -> None:
    """The reader's 6/10-byte progress format."""
    data = struct.pack("<HHH", spine, page, page_count)
    if visible_text_offset is not None:
        data += struct.pack("<I", visible_text_offset)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)


def read_progress_bin(path: Path) -> dict | None:
    if not path.exists():
        return None
    raw = path.read_bytes()
    if len(raw) not in (4, 6, 10):
        return None
    spine, page = struct.unpack_from("<HH", raw)
    out = {"spine": spine, "page": page}
    if len(raw) >= 6:
        out["pageCount"] = struct.unpack_from("<H", raw, 4)[0]
    if len(raw) == 10:
        out["visibleTextOffset"] = struct.unpack_from("<I", raw, 6)[0]
    return out


# --- reader stores (src/ClippingStore.cpp, src/BookmarkStore.cpp) and their BookOrbit
# sync state (lib/BookOrbitSync/BookOrbit*Store.cpp) ------------------------------------

CHAPTER_TITLE_MAX = 48
SNIPPET_MAX = 64


def _pack_str(s: str) -> bytes:
    data = s.encode()
    return struct.pack("<I", len(data)) + data


def _read_str(raw: bytes, offset: int) -> tuple[str, int]:
    (length,) = struct.unpack_from("<I", raw, offset)
    offset += 4
    return raw[offset:offset + length].decode(errors="replace"), offset + length


def _fixed_str(s: str, size: int) -> bytes:
    return s.encode()[:size - 1].ljust(size, b"\0")


def write_clipping_store(path: Path, title: str, author: str, book_path: str,
                         clippings: list[dict]) -> None:
    """Clipping store v4: header then per record 8×u16, u32 timestamp,
    u32 layoutSignature, u16 tableSelection, char[48] chapter, u16 textLen, text.

    A highlight made on the device covers at least one word, so give such a clipping a
    nonzero "wordCount": the firmware tells the ones it received from the server by
    their empty word range, and never uploads those."""
    blob = struct.pack("<BH", 4, len(clippings))
    blob += _pack_str(title) + _pack_str(author) + _pack_str(book_path)
    for c in clippings:
        text = c["text"].encode()
        word_count = c.get("wordCount", 0)
        blob += struct.pack("<8H", c["spine"], c.get("startPage", 0), c.get("endPage", 0),
                            c.get("pageCount", 0), 0, max(word_count - 1, 0), word_count,
                            c.get("paragraph", 0xFFFF))
        blob += struct.pack("<II", c["timestamp"], c.get("layoutSignature", 0))
        blob += struct.pack("<H", c.get("tableSelection", 0xFFFF))  # UINT16_MAX: not a table cell
        blob += _fixed_str(c.get("chapter", ""), CHAPTER_TITLE_MAX)
        blob += struct.pack("<H", len(text)) + text
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(blob)


def read_clipping_store(path: Path) -> list[dict]:
    if not path.exists():
        return []
    raw = path.read_bytes()
    version, count = struct.unpack_from("<BH", raw, 0)
    assert version in (3, 4), f"unexpected clipping store version {version} in {path}"
    offset = 3
    title, offset = _read_str(raw, offset)
    author, offset = _read_str(raw, offset)
    book_path, offset = _read_str(raw, offset)
    out = []
    for _ in range(count):
        spine, _sp, _ep, _pc, _sw, _ew, word_count, paragraph = struct.unpack_from("<8H", raw, offset)
        offset += 16
        timestamp, _sig = struct.unpack_from("<II", raw, offset)
        offset += 8
        if version >= 4:
            offset += 2  # tableSelection (u16), irrelevant to the exchanged text
        chapter = raw[offset:offset + CHAPTER_TITLE_MAX].split(b"\0", 1)[0].decode(errors="replace")
        offset += CHAPTER_TITLE_MAX
        (text_len,) = struct.unpack_from("<H", raw, offset)
        offset += 2
        text = raw[offset:offset + text_len].decode(errors="replace")
        offset += text_len
        out.append({"spine": spine, "paragraph": paragraph, "timestamp": timestamp,
                    "chapter": chapter, "text": text, "wordCount": word_count})
    return out


def write_bookmark_store(path: Path, title: str, author: str, book_path: str,
                         bookmarks: list[dict]) -> None:
    """Bookmark store v5: header then per record u16 spine, f32 progress,
    u32 timestamp, char[48] chapter, u16 paragraph, char[64] snippet."""
    blob = struct.pack("<BH", 5, len(bookmarks))
    blob += _pack_str(title) + _pack_str(author) + _pack_str(book_path)
    for b in bookmarks:
        blob += struct.pack("<HfI", b["spine"], b.get("progress", 0.0), b["timestamp"])
        blob += _fixed_str(b.get("chapter", ""), CHAPTER_TITLE_MAX)
        blob += struct.pack("<H", b.get("paragraph", 0xFFFF))
        blob += _fixed_str(b.get("snippet", ""), SNIPPET_MAX)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(blob)


def read_bookmark_store(path: Path) -> list[dict]:
    if not path.exists():
        return []
    raw = path.read_bytes()
    version, count = struct.unpack_from("<BH", raw, 0)
    assert version == 5, f"unexpected bookmark store version {version} in {path}"
    offset = 3
    for _ in range(3):  # title, author, path
        _, offset = _read_str(raw, offset)
    out = []
    for _ in range(count):
        spine, progress, timestamp = struct.unpack_from("<HfI", raw, offset)
        offset += 10
        chapter = raw[offset:offset + CHAPTER_TITLE_MAX].split(b"\0", 1)[0].decode(errors="replace")
        offset += CHAPTER_TITLE_MAX
        (paragraph,) = struct.unpack_from("<H", raw, offset)
        offset += 2
        snippet = raw[offset:offset + SNIPPET_MAX].split(b"\0", 1)[0].decode(errors="replace")
        offset += SNIPPET_MAX
        out.append({"spine": spine, "progress": progress, "timestamp": timestamp,
                    "chapter": chapter, "paragraph": paragraph, "snippet": snippet})
    return out


def write_boa_store(path: Path, watermark: int, records: list[dict]) -> None:
    """BookOrbit annotation sync-state store ("BOA1")."""
    blob = b"BOA1" + struct.pack("<I", watermark)
    for r in records:
        pos0, pos1 = r["pos0"].encode(), r["pos1"].encode()
        blob += struct.pack("<IIHHHH", r["timestamp"], r["identityEpoch"], r["spine"],
                            r.get("paragraph", 0xFFFF), len(pos0), len(pos1))
        blob += pos0 + pos1
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(blob)


def read_boa_store(path: Path) -> tuple[int, list[dict]]:
    if not path.exists():
        return 0, []
    raw = path.read_bytes()
    assert raw[:4] == b"BOA1", f"unexpected annotation store magic in {path}"
    (watermark,) = struct.unpack_from("<I", raw, 4)
    offset, out = 8, []
    while offset < len(raw):
        ts, epoch, spine, paragraph, len0, len1 = struct.unpack_from("<IIHHHH", raw, offset)
        offset += 16
        pos0 = raw[offset:offset + len0].decode(errors="replace")
        offset += len0
        pos1 = raw[offset:offset + len1].decode(errors="replace")
        offset += len1
        out.append({"timestamp": ts, "identityEpoch": epoch, "spine": spine,
                    "paragraph": paragraph, "pos0": pos0, "pos1": pos1})
    return watermark, out


def write_bob_store(path: Path, watermark: int, records: list[dict], magic: bytes = b"BOB2") -> None:
    """BookOrbit bookmark sync-state store. BOB1, the same layout, is what builds before the
    fix for issue #120 wrote (see BookOrbitBookmarkStore::isFirstVersion)."""
    blob = magic + struct.pack("<I", watermark)
    for r in records:
        pos = r["pos"].encode()
        blob += struct.pack("<IIHH", r["timestamp"], r["identityEpoch"], r["spine"], len(pos))
        blob += pos
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(blob)


def read_bob_store(path: Path) -> tuple[int, list[dict]]:
    if not path.exists():
        return 0, []
    raw = path.read_bytes()
    assert raw[:4] in (b"BOB1", b"BOB2"), f"unexpected bookmark store magic in {path}"
    (watermark,) = struct.unpack_from("<I", raw, 4)
    offset, out = 8, []
    while offset < len(raw):
        ts, epoch, spine, pos_len = struct.unpack_from("<IIHH", raw, offset)
        offset += 12
        pos = raw[offset:offset + pos_len].decode(errors="replace")
        offset += pos_len
        out.append({"timestamp": ts, "identityEpoch": epoch, "spine": spine, "pos": pos})
    return watermark, out


def bob_store_magic(path: Path) -> bytes:
    return path.read_bytes()[:4] if path.exists() else b""


def local_highlight(text: str, timestamp: int, epoch: int, spine: int = 1, paragraph: int = 2,
                    length: int | None = None) -> tuple[dict, dict]:
    """A highlight made on the device, as the reader saves it: a clipping and its minted
    position record, joined by `timestamp` (the reader's millis()/1000 uptime stamp; any
    nonzero one will do). `epoch` is the record's WallClock identity date, which the server
    validates. The position spans `length` characters (the text's own by default) from the
    start of the chapter's paragraph."""
    xpath = f"/body/DocFragment[{spine + 1}]/body/p[{paragraph}]/text()"
    clipping = {"spine": spine, "paragraph": paragraph, "timestamp": timestamp,
                "text": text, "chapter": "Chapter", "wordCount": len(text.split())}
    record = {"timestamp": timestamp, "identityEpoch": epoch, "spine": spine, "paragraph": paragraph,
              "pos0": f"{xpath}.0", "pos1": f"{xpath}.{len(text) if length is None else length}"}
    return clipping, record


# --- download index (/.crosspoint/bookorbit_downloads.bin, docs/file-formats.md) --------
# Its header carries the CRC32 of the configured server URL, which the firmware keeps verbatim.


def write_download_index(path: Path, server_url: str, entries: list[tuple[int, str, int]],
                         version: int) -> None:
    """Writes (bookId, sdPath, fileSize) entries as a BOD1 or BOD2 index."""
    out = bytearray(f"BOD{version}".encode()) + struct.pack("<I", zlib.crc32(server_url.encode()))
    if version == 1:
        for book_id, sd_path, size in entries:
            raw = sd_path.encode()
            out += struct.pack("<qIH", book_id, size, len(raw)) + raw
    else:
        out += struct.pack("<I", len(entries))
        offset = 12 + 22 * len(entries)
        for book_id, sd_path, size in entries:
            raw = sd_path.encode()
            out += struct.pack("<qIIIH", book_id, size, fnv1a32(raw), offset, len(raw))
            offset += len(raw)
        for _, sd_path, _ in entries:
            out += sd_path.encode()
    path.write_bytes(bytes(out))


def read_download_index(path: Path, server_url: str) -> list[tuple[int, str, int]]:
    """Parses a BOD2 index into (bookId, sdPath, fileSize) entries, checking every field
    the format documents: magic, server CRC, count, and each path's hash and offset."""
    data = path.read_bytes()
    assert data[:4] == b"BOD2", f"download index is not BOD2: {data[:4]!r}"
    crc, count = struct.unpack_from("<II", data, 4)
    assert crc == zlib.crc32(server_url.encode()), "download index records another server"
    entries = []
    for i in range(count):
        book_id, size, fnv, offset, length = struct.unpack_from("<qIIIH", data, 12 + 22 * i)
        raw = data[offset:offset + length]
        assert len(raw) == length, f"entry {i} points past the end of the file"
        assert fnv == fnv1a32(raw), f"entry {i} path hash does not match its path"
        entries.append((book_id, raw.decode(), size))
    assert len(data) == 12 + 22 * count + sum(len(p.encode()) for _, p, _ in entries), \
        "download index has trailing bytes"
    return entries


# --- catalog list cache (/.crosspoint/bookorbit_lists/*.json, BookOrbitCatalogListCache) --


def read_list_caches(folder: Path) -> list[dict]:
    return [json.loads(p.read_text()) for p in sorted(folder.glob("*.json"))]


def book_listings(caches: list[dict]) -> list[dict]:
    """The cached book listings, without the root (which carries its own totals)."""
    return [c for c in caches if "total" in c and "sections" not in c]


def listings_by_key(caches: list[dict], first: int, last: int) -> dict[tuple[str, ...], dict]:
    """The cached book listings by a slice of their cache key, which reads
    books|page|sort|readStatus|order|shelf|..."""
    return {tuple(c["key"].split("|")[first:last]): c for c in caches if c.get("key", "").startswith("books|")}
