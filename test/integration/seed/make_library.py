#!/usr/bin/env python3
"""Generate the integration-test EPUB library.

Takes a small fixture EPUB from test/epubs/ and stamps out N variants, each
with a unique title, author and identifier written into the OPF. BookOrbit
identifies books by KOReader's partial MD5 of the file bytes, so uniqueness of
that hash is asserted for every variant, not assumed.

Output:
  test/integration/library/        the EPUBs (local reference copy; seed.py
                                   copies them into the server's book dock,
                                   which consumes its files on finalize)
  test/integration/library.json    per-book {file, title, author, hash}
"""

from __future__ import annotations

import argparse
import json
import re
import shutil
import sys
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from kosync import partial_md5  # noqa: E402

ROOT = Path(__file__).resolve().parents[3]
INTEGRATION = ROOT / "test" / "integration"
TEMPLATE = ROOT / "test" / "epubs" / "test_reader_rendering_matrix.epub"

AUTHORS = ["Ada Byron", "Blaise Cendrars", "Colette Vivier", "Denis Diderot",
           "Emilia Pardo", "Franz Kafka", "George Sand", "Hugo Verne"]


def make_variant(template: Path, dest: Path, index: int) -> dict:
    title = f"Integration Book {index:03d}"
    author = AUTHORS[index % len(AUTHORS)]
    uid = f"crossink-integration-{index:03d}"

    with zipfile.ZipFile(template) as src, zipfile.ZipFile(dest, "w") as out:
        for item in src.infolist():
            data = src.read(item.filename)
            if item.filename == "META-INF/container.xml":
                # container.xml sits within the first sampled kilobyte of the zip,
                # so this comment is what actually guarantees a unique partial MD5.
                data = data.replace(b"?>", f"?><!-- {uid} -->".encode(), 1)
            if item.filename.endswith(".opf"):
                text = data.decode("utf-8")
                text = re.sub(r"<dc:title>[^<]*</dc:title>", f"<dc:title>{title}</dc:title>", text, count=1)
                if "<dc:creator" in text:
                    text = re.sub(r"(<dc:creator[^>]*>)[^<]*(</dc:creator>)", rf"\g<1>{author}\g<2>", text, count=1)
                else:
                    text = text.replace("</dc:title>", f"</dc:title><dc:creator>{author}</dc:creator>", 1)
                text = re.sub(r"(<dc:identifier[^>]*>)[^<]*(</dc:identifier>)", rf"\g<1>{uid}\g<2>", text, count=1)
                data = text.encode("utf-8")
            # mimetype must stay first and uncompressed for a valid EPUB.
            compress = zipfile.ZIP_STORED if item.filename == "mimetype" else zipfile.ZIP_DEFLATED
            out.writestr(item, data, compress_type=compress)

    return {"file": dest.name, "title": title, "author": author, "hash": partial_md5(dest)}


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
        entry = make_variant(args.template, library / f"integration-book-{i:03d}.epub", i)
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
