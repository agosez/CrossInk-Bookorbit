#!/usr/bin/env python3
"""Generate the integration-test EPUB library.

Takes a small fixture EPUB from test/epubs/ and stamps out N variants, each
with a unique title, author and identifier written into the OPF. BookOrbit
identifies books by KOReader's partial MD5 of the file bytes, so uniqueness of
that hash is asserted for every variant, not assumed. Every book except each
10th gets a generated PNG cover, so the server has thumbnails to serve.

Output:
  test/integration/library/        the EPUBs (local reference copy; seed.py
                                   copies them into the server's book dock,
                                   which consumes its files on finalize)
  test/integration/library.json    per-book {file, title, author, hash, cover}
"""

from __future__ import annotations

import argparse
import json
import re
import shutil
import struct
import sys
import zipfile
import zlib
from pathlib import Path
from posixpath import dirname as zip_dirname

sys.path.insert(0, str(Path(__file__).resolve().parent))
from kosync import partial_md5  # noqa: E402

ROOT = Path(__file__).resolve().parents[3]
INTEGRATION = ROOT / "test" / "integration"
TEMPLATE = ROOT / "test" / "epubs" / "test_reader_rendering_matrix.epub"

AUTHORS = ["Ada Byron", "Blaise Cendrars", "Colette Vivier", "Denis Diderot",
           "Emilia Pardo", "Franz Kafka", "George Sand", "Hugo Verne"]


def _png_chunk(kind: bytes, payload: bytes) -> bytes:
    return struct.pack(">I", len(payload)) + kind + payload + struct.pack(">I", zlib.crc32(kind + payload))


def make_cover_png(index: int, width: int = 300, height: int = 450) -> bytes:
    """A dependency-free PNG cover: a per-book gray shade with darker bands, so
    covers are visually distinct and the reader's dithering has real content."""
    shade = 90 + (index * 37) % 130
    rows = bytearray()
    for y in range(height):
        banded = y % 90 < 14 or abs(y - (index * 13) % height) < 8
        value = max(0, shade - 70) if banded else shade
        rows += b"\x00" + bytes([value, (value + index) % 256, 255 - value]) * width
    return (b"\x89PNG\r\n\x1a\n"
            + _png_chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
            + _png_chunk(b"IDAT", zlib.compress(bytes(rows), 6))
            + _png_chunk(b"IEND", b""))


def add_cover(text: str, cover_href: str) -> str:
    """Declare cover.png in the OPF: an EPUB3 cover-image manifest item plus the
    EPUB2 <meta name="cover"> pointer, so any extractor finds it."""
    item = f'<item id="crossink-cover" href="{cover_href}" media-type="image/png" properties="cover-image"/>'
    text = text.replace("</manifest>", item + "</manifest>", 1)
    return text.replace("</metadata>", '<meta name="cover" content="crossink-cover"/></metadata>', 1)


def make_variant(template: Path, dest: Path, index: int, with_cover: bool) -> dict:
    title = f"Integration Book {index:03d}"
    author = AUTHORS[index % len(AUTHORS)]
    uid = f"crossink-integration-{index:03d}"

    with zipfile.ZipFile(template) as src, zipfile.ZipFile(dest, "w") as out:
        opf_dir = ""
        for item in src.infolist():
            data = src.read(item.filename)
            if item.filename == "META-INF/container.xml":
                # container.xml sits within the first sampled kilobyte of the zip,
                # so this comment is what actually guarantees a unique partial MD5.
                data = data.replace(b"?>", f"?><!-- {uid} -->".encode(), 1)
            if item.filename.endswith(".opf"):
                opf_dir = zip_dirname(item.filename)
                text = data.decode("utf-8")
                text = re.sub(r"<dc:title>[^<]*</dc:title>", f"<dc:title>{title}</dc:title>", text, count=1)
                if "<dc:creator" in text:
                    text = re.sub(r"(<dc:creator[^>]*>)[^<]*(</dc:creator>)", rf"\g<1>{author}\g<2>", text, count=1)
                else:
                    text = text.replace("</dc:title>", f"</dc:title><dc:creator>{author}</dc:creator>", 1)
                text = re.sub(r"(<dc:identifier[^>]*>)[^<]*(</dc:identifier>)", rf"\g<1>{uid}\g<2>", text, count=1)
                if with_cover:
                    text = add_cover(text, "cover.png")
                data = text.encode("utf-8")
            # mimetype must stay first and uncompressed for a valid EPUB.
            compress = zipfile.ZIP_STORED if item.filename == "mimetype" else zipfile.ZIP_DEFLATED
            out.writestr(item, data, compress_type=compress)
        if with_cover:
            # Next to the OPF, where its relative href points.
            cover_name = f"{opf_dir}/cover.png" if opf_dir else "cover.png"
            out.writestr(cover_name, make_cover_png(index), compress_type=zipfile.ZIP_STORED)

    return {"file": dest.name, "title": title, "author": author, "hash": partial_md5(dest),
            "cover": with_cover}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--count", type=int, default=100)
    parser.add_argument("--template", type=Path, default=TEMPLATE)
    args = parser.parse_args()

    library = INTEGRATION / "library"
    if library.exists():
        shutil.rmtree(library)
    library.mkdir(parents=True)

    books = []
    for i in range(1, args.count + 1):
        # Most books carry a generated cover; every 10th stays coverless so the
        # catalog's placeholder path is exercised alongside real thumbnails.
        entry = make_variant(args.template, library / f"integration-book-{i:03d}.epub", i,
                             with_cover=i % 10 != 0)
        books.append(entry)

    hashes = [b["hash"] for b in books]
    duplicates = {h for h in hashes if hashes.count(h) > 1}
    if duplicates:
        print(f"ERROR: {len(duplicates)} duplicate partial-MD5 hashes; the OPF edits "
              "did not land inside the sampled chunks of this template.", file=sys.stderr)
        return 1

    (INTEGRATION / "library.json").write_text(json.dumps(books, indent=2))
    print(f"Generated {len(books)} books in {library} (all hashes unique)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
