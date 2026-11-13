#!/usr/bin/env python3
"""SD-card kit that stresses one BookOrbit sync on a real device, against the
integration server (see README.md) behind a Caddy reverse proxy.

One book gets, all at once:
  - local highlights waiting to upload (clipping store + BookOrbit position records,
    listed out of datetime order in the file, as a clock set back would leave them);
  - queued reading events (the page-stats queue);
  - highlights on the server from another device, for the device to pull.

The library's fixtures are too short for that many highlights without overlaps, so the
kit generates its own book (20 chapters) and uploads it to the server. The book is
byte-for-byte the same for a given --variant, so a second run finds it there already.
Once a device has synced a variant, the server remembers what that device took: take
the next variant for a fresh start. Every run adds --server highlights to the book.

The kit folder mirrors the SD card root: copy books/ and .crosspoint/ onto the card,
merging .crosspoint with what is already there.

  python3 test/integration/device_kit.py --out ~/kit --start-caddy
"""
from __future__ import annotations

import argparse
import base64
import json
import random
import re
import shutil
import ssl
import struct
import subprocess
import sys
import time
import urllib.request
import xml.etree.ElementTree as ET
import zipfile
from pathlib import Path, PurePosixPath

INTEGRATION = Path(__file__).resolve().parent
sys.path.insert(0, str(INTEGRATION / "seed"))
sys.path.insert(0, str(INTEGRATION / "harness"))
from kosync import BASE_URL, AdminClient, KosyncDevice, kodatetime, partial_md5  # noqa: E402
from run_scenarios import load_seed, write_boa_store, write_clipping_store  # noqa: E402

XHTML = "{http://www.w3.org/1999/xhtml}"
KIT_PEER_ID = "crossink-device-kit-peer"
CADDY_IMAGE = "caddy:2.10.2"
CADDY_CONTAINER = "crossink-kit-caddy"
COMPOSE_NETWORK = "crossink-integration_default"
STATS_PROGRESS_SCALE = 10000
CHAPTERS = 20
PARAGRAPHS_PER_CHAPTER = 14

WORDS = """river morning garden letter window evening village silence harbor journey
lantern mountain orchard candle meadow winter summer autumn spring forest valley
station bridge market courtyard library notebook compass island lighthouse storm
quiet distant golden narrow ancient gentle bright hollow restless patient careful
walked waited listened wondered remembered carried opened followed watched noticed
slowly softly quickly almost already always never often perhaps nearly suddenly
the a an of to in on under over beyond between toward across through along with
and but while because although until since before after when where as if""".split()


# --- the book -------------------------------------------------------------------------


def _sentence(rng: random.Random) -> str:
    words = [rng.choice(WORDS) for _ in range(rng.randint(8, 16))]
    return " ".join(words).capitalize() + "."


def _xhtml(title: str, body: str) -> str:
    return ('<?xml version="1.0" encoding="UTF-8"?>\n<!DOCTYPE html>\n'
            '<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops" lang="en">\n'
            f"<head><title>{title}</title></head>\n<body>\n{body}\n</body>\n</html>\n")


def write_book(path: Path, title: str, variant: int) -> None:
    """A plain EPUB 3 whose bytes depend only on `variant`: fixed text and fixed zip
    dates, so its content hash (what the server and the device key the book by) is stable."""
    rng = random.Random(f"kit-{variant}")
    files: list[tuple[str, str]] = [
        ("META-INF/container.xml",
         '<?xml version="1.0"?>\n<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">'
         '<rootfiles><rootfile full-path="OEBPS/content.opf" media-type="application/oebps-package+xml"/>'
         "</rootfiles></container>\n")]
    manifest, spine, nav = [], [], []
    for c in range(1, CHAPTERS + 1):
        paragraphs = "\n".join(f"<p>{' '.join(_sentence(rng) for _ in range(rng.randint(4, 6)))}</p>"
                               for _ in range(PARAGRAPHS_PER_CHAPTER))
        files.append((f"OEBPS/chapter{c}.xhtml", _xhtml(f"Chapter {c}", f"<h1>Chapter {c}</h1>\n{paragraphs}")))
        manifest.append(f'<item id="c{c}" href="chapter{c}.xhtml" media-type="application/xhtml+xml"/>')
        spine.append(f'<itemref idref="c{c}"/>')
        nav.append(f'<li><a href="chapter{c}.xhtml">Chapter {c}</a></li>')
    files.append(("OEBPS/nav.xhtml", _xhtml("Contents", f'<nav epub:type="toc"><ol>{"".join(nav)}</ol></nav>')))
    files.append(("OEBPS/content.opf", f"""<?xml version="1.0" encoding="UTF-8"?>
<package xmlns="http://www.idpf.org/2007/opf" version="3.0" unique-identifier="uid">
<metadata xmlns:dc="http://purl.org/dc/elements/1.1/">
<dc:identifier id="uid">urn:uuid:00000000-0000-4000-8000-{variant:012d}</dc:identifier>
<dc:title>{title}</dc:title><dc:creator>CrossInk Kit</dc:creator><dc:language>en</dc:language>
<meta property="dcterms:modified">2026-01-01T00:00:00Z</meta>
</metadata>
<manifest><item id="nav" href="nav.xhtml" media-type="application/xhtml+xml" properties="nav"/>
{chr(10).join(manifest)}</manifest>
<spine>{"".join(spine)}</spine>
</package>
"""))
    with zipfile.ZipFile(path, "w") as epub:
        epub.writestr(zipfile.ZipInfo("mimetype", (2026, 1, 1, 0, 0, 0)), "application/epub+zip",
                      compress_type=zipfile.ZIP_STORED)
        for name, content in files:
            epub.writestr(zipfile.ZipInfo(name, (2026, 1, 1, 0, 0, 0)), content, compress_type=zipfile.ZIP_DEFLATED)


def ensure_on_server(manifest: dict, path: Path, book_hash: str, peer: KosyncDevice) -> None:
    """Upload the book unless the server has it, then wait until the sync API knows its hash."""
    probe = peer.exchange_annotations(book_hash, keys=[], keys_complete=False, changes=[])
    if not probe.get("unmatched"):
        return
    admin = AdminClient(BASE_URL)
    admin.login(manifest["user"]["username"], manifest["user"]["password"])
    library_id, _ = admin.ensure_library("Integration", "/data/library")
    admin.upload_book(library_id, path)
    deadline = time.time() + 120
    while peer.exchange_annotations(book_hash, keys=[], keys_complete=False, changes=[]).get("unmatched"):
        if time.time() > deadline:
            raise SystemExit(f"the server never matched {path.name} (hash {book_hash})")
        time.sleep(2)


# --- the device's stores ----------------------------------------------------------------


def spine_documents(epub: zipfile.ZipFile) -> list[str]:
    container = ET.fromstring(epub.read("META-INF/container.xml"))
    opf_path = next(e.get("full-path") for e in container.iter() if e.tag.endswith("rootfile"))
    opf = ET.fromstring(epub.read(opf_path))
    base = PurePosixPath(opf_path).parent
    items = {e.get("id"): e.get("href") for e in opf.iter() if e.tag.endswith("item")}
    return [str(base / items[e.get("idref")]) for e in opf.iter() if e.tag.endswith("itemref")]


def excerpts(epub_path: Path) -> list[dict]:
    """Word-aligned, non-overlapping slices of each chapter's top-level paragraphs, with
    the xpointers of their first text node: what a highlight made on the device carries."""
    out = []
    with zipfile.ZipFile(epub_path) as epub:
        for spine, doc in enumerate(spine_documents(epub)):
            body = ET.fromstring(epub.read(doc)).find(f"{XHTML}body")
            if body is None:
                continue
            heading = body.find(f"{XHTML}h1")
            chapter = "".join(heading.itertext()).strip() if heading is not None else f"Chapter {spine + 1}"
            for p_index, p in enumerate(body.findall(f"{XHTML}p"), start=1):
                node = p.text  # text() names the first text node: skip paragraphs not opening on text
                if not node or not node.strip():
                    continue
                words = [(m.start(), m.end()) for m in re.finditer(r"\S+", node)]
                i = 0
                while i + 6 <= len(words):
                    n = 6 + (i + spine) % 7  # 6 to 12 words
                    span = words[i:i + n]
                    start, end = span[0][0], span[-1][1]
                    xpath = f"/body/DocFragment[{spine + 1}]/body/p[{p_index}]/text()"
                    out.append({"spine": spine, "paragraph": p_index, "chapter": chapter[:47],
                                "text": node[start:end], "words": len(span),
                                "pos0": f"{xpath}.{start}", "pos1": f"{xpath}.{end}"})
                    i += n + 2  # a gap, so no two highlights touch
    return out


def reading_events(count: int, now: int, rng: random.Random) -> bytes:
    """BookOrbitStatsQueue's file: "BOQ1", then 16-byte events in chronological order,
    in sessions of 25 pages over the last 30 days, the position moving forward. Exact
    timestamps (no CLOCK_APPROXIMATE flag), so none is corrected or dropped at upload."""
    blob = bytearray(b"BOQ1")
    sessions = max(1, (count + 24) // 25)
    starts = sorted(rng.sample(range(now - 30 * 86400, now - 3600, 3600), sessions))
    written = 0
    for start in starts:
        t = start
        for _ in range(min(25, count - written)):
            dwell = rng.randint(25, 120)
            position = 100 + (STATS_PROGRESS_SCALE - 600) * written // max(1, count - 1)
            blob += struct.pack("<IIHHHBB", t, dwell, position, STATS_PROGRESS_SCALE, 0, 0, 0)
            t += dwell + rng.randint(1, 5)
            written += 1
    return bytes(blob)


def obfuscate(mac: bytes, plaintext: str) -> str:
    """obfuscation::obfuscateToBase64 for a given device: "CPV1" + FNV1a32(MAC || plaintext)
    + plaintext, XOR MAC, base64. The MAC is the ESP32 base MAC, the Wi-Fi station one."""
    h = 2166136261
    for b in mac + plaintext.encode():
        h = ((h ^ b) * 16777619) & 0xFFFFFFFF
    payload = b"CPV1" + struct.pack("<I", h) + plaintext.encode()
    return base64.b64encode(bytes(c ^ mac[i % len(mac)] for i, c in enumerate(payload))).decode()


# --- Caddy ------------------------------------------------------------------------------


def write_caddyfile(path: Path, host: str, port: int) -> None:
    # default_sni: the device names the server by IP, TLS carries no name for an IP, and
    # Caddy needs telling which certificate answers such a handshake.
    path.write_text(f"""{{
\tdefault_sni {host}
\tskip_install_trust
\tadmin off
}}

https://{host}:{port} {{
\ttls internal
\treverse_proxy bookorbit:3000
}}
""")


def start_caddy(caddyfile: Path, port: int) -> None:
    subprocess.run(["docker", "rm", "-f", CADDY_CONTAINER], capture_output=True)
    subprocess.run(["docker", "run", "-d", "--name", CADDY_CONTAINER, "--network", COMPOSE_NETWORK,
                    "-p", f"{port}:{port}", "-v", f"{caddyfile}:/etc/caddy/Caddyfile:ro", CADDY_IMAGE],
                   check=True, capture_output=True)


def wait_healthy(url: str, timeout_s: float = 30.0) -> None:
    context = ssl.create_default_context()
    context.check_hostname = False
    context.verify_mode = ssl.CERT_NONE  # Caddy's internal CA; the firmware does not verify either
    deadline = time.time() + timeout_s
    while True:
        try:
            with urllib.request.urlopen(f"{url}/api/v1/health", context=context, timeout=5) as resp:
                if resp.status == 200:
                    return
        except OSError as error:
            if time.time() > deadline:
                raise SystemExit(f"{url} is not answering through Caddy: {error}")
        time.sleep(1)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--out", type=Path, required=True, help="kit folder (SD card root layout)")
    parser.add_argument("--variant", type=int, default=1, help="which generated book (default 1)")
    parser.add_argument("--local", type=int, default=100, help="highlights to upload (default 100)")
    parser.add_argument("--server", type=int, default=60, help="highlights to pull from the server (default 60)")
    parser.add_argument("--events", type=int, default=600, help="reading events to upload (default 600)")
    parser.add_argument("--host", default=None, help="this Mac's LAN address (default: en0's)")
    parser.add_argument("--port", type=int, default=8443)
    parser.add_argument("--mac", help="device MAC (AA:BB:CC:DD:EE:FF), to write bookorbit.json")
    parser.add_argument("--start-caddy", action="store_true", help="(re)start the Caddy container")
    args = parser.parse_args()
    if args.local > 256 or args.local + args.server > 256:
        raise SystemExit("a book holds at most 256 highlights on the device")

    manifest, _ = load_seed()
    host = args.host or subprocess.run(["ipconfig", "getifaddr", "en0"], capture_output=True,
                                       text=True).stdout.strip()
    if not host:
        raise SystemExit("no LAN address on en0; pass --host")
    url = f"https://{host}:{args.port}"

    out = args.out.expanduser().resolve()
    title = f"CrossInk Sync Kit {args.variant}"
    book_path = out / "books" / f"crossink-sync-kit-{args.variant}.epub"
    book_path.parent.mkdir(parents=True, exist_ok=True)
    write_book(book_path, title, args.variant)
    book_hash = partial_md5(book_path)

    peer = KosyncDevice(BASE_URL, manifest["kosync"]["username"], manifest["kosync"]["password"], KIT_PEER_ID)
    peer.auth()
    ensure_on_server(manifest, book_path, book_hash, peer)

    slices = excerpts(book_path)
    rng = random.Random(f"kit-{args.variant}-{time.time_ns()}")
    rng.shuffle(slices)
    if len(slices) < args.local + args.server:
        raise SystemExit(f"only {len(slices)} excerpts; lower --local/--server")
    local, remote = slices[:args.local], slices[args.local:args.local + args.server]
    now = int(time.time())

    crosspoint = out / ".crosspoint"
    state_dir = crosspoint / f"book_{book_hash}"
    if state_dir.exists():
        shutil.rmtree(state_dir)
    state_dir.mkdir(parents=True)

    # Local highlights: distinct datetimes over the last 20 days, joined to their clippings
    # by an uptime-style timestamp, and listed in shuffled order.
    epochs = sorted(rng.sample(range(now - 20 * 86400, now - 3600), args.local))
    clippings, records = [], []
    for i, (s, epoch) in enumerate(zip(local, epochs)):
        clippings.append({"spine": s["spine"], "paragraph": s["paragraph"], "timestamp": 1000 + i,
                          "text": s["text"], "chapter": s["chapter"], "wordCount": s["words"]})
        records.append({"timestamp": 1000 + i, "identityEpoch": epoch, "spine": s["spine"],
                        "paragraph": s["paragraph"], "pos0": s["pos0"], "pos1": s["pos1"]})
    rng.shuffle(records)
    write_clipping_store(crosspoint / "clippings" / f"epub_{book_hash}.bin", title, "CrossInk Kit",
                         f"/books/{book_path.name}", clippings)
    write_boa_store(state_dir / "bookorbit_annotations.bin", 0, records)
    (state_dir / "bookorbit_stats.bin").write_bytes(reading_events(args.events, now, rng))

    # Server highlights, made by a peer device, older than the local ones.
    remote_epochs = sorted(rng.sample(range(now - 40 * 86400, now - 21 * 86400), args.server))
    for start in range(0, args.server, 20):
        changes = [{"datetime": kodatetime(epoch), "pos0": s["pos0"], "pos1": s["pos1"], "text": s["text"],
                    "chapter": s["chapter"]}
                   for s, epoch in zip(remote[start:start + 20], remote_epochs[start:start + 20])]
        peer.exchange_annotations(book_hash, keys=[], keys_complete=False, changes=changes)

    if args.mac:
        mac = bytes(int(part, 16) for part in args.mac.replace("-", ":").split(":"))
        if len(mac) != 6:
            raise SystemExit(f"not a MAC: {args.mac}")
        (out / "bookorbit.json").write_text(json.dumps({
            "username": manifest["kosync"]["username"],
            "password_obf": obfuscate(mac, manifest["kosync"]["password"]),
            "serverUrl": url,
            "syncBehavior": 1,
        }, indent=2))

    write_caddyfile(out / "Caddyfile", host, args.port)
    if args.start_caddy:
        start_caddy(out / "Caddyfile", args.port)
        wait_healthy(url)

    print(f"Book        {title} ({book_path.name}, hash {book_hash})")
    print(f"To upload   {args.local} highlights, {args.events} reading events")
    print(f"To pull     {args.server} highlights from {KIT_PEER_ID}")
    print(f"Server      {url}  user {manifest['kosync']['username']}  password {manifest['kosync']['password']}")
    print(f"Kit         {out}")


if __name__ == "__main__":
    main()
