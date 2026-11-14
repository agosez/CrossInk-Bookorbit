#!/usr/bin/env python3
"""Export this fork's language and hyphenation downloads into a static site tree.

The release workflows run this against a checkout of the gh-pages branch, which GitHub
Pages serves at the firmware's ASSET_DOWNLOAD_BASE_URL. It writes the same paths the
firmware requests from upstream's S3 host (scripts/publish_language_assets.py):
immutable snapshots/<revision>/... files, then the per-contract catalogs that point at
them. Snapshots are only ever added, so firmware built from an older commit keeps
finding the files its catalog names.

Each catalog is signed in an envelope served beside it, "<catalog>.json.sig", in the format
src/network/AssetCatalogSignature.h describes. The firmware refuses an unsigned catalog: on
the X3 and X4 these downloads arrive over a transport that does not check certificates.
"""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import sys
from types import SimpleNamespace

import publish_language_assets as publisher

CATALOG_LIMITS = {'translation': 48, 'hyphenation': 9}
SIGNATURE_HEADER = b'CrossInk signed asset catalog 1\n'
SIGNATURE_SUFFIX = '.sig'
FIRMWARE_KEY_SOURCE = Path(publisher.root) / 'src/network/AssetCatalogSignature.cpp'


def firmware_public_key():
    """The catalog key the firmware trusts: CROSSINK_ASSET_CATALOG_KEY when set, else the built-in one."""
    override = os.environ.get('CROSSINK_ASSET_CATALOG_KEY')
    if override:
        return override
    match = re.search(r'#define ASSET_CATALOG_PUBLIC_KEY((?:\s*\\?\s*"[0-9a-f]*")+)', FIRMWARE_KEY_SOURCE.read_text())
    if not match:
        raise RuntimeError('cannot find ASSET_CATALOG_PUBLIC_KEY in ' + str(FIRMWARE_KEY_SOURCE))
    return ''.join(re.findall(r'"([0-9a-f]*)"', match.group(1)))


def public_key_hex(key_path):
    der = subprocess.run(['openssl', 'ec', '-in', str(key_path), '-pubout', '-conv_form', 'uncompressed',
                          '-outform', 'DER'], capture_output=True, check=True).stdout
    return der[-65:].hex()


def envelope(kind, catalog_bytes, notices_bytes, sources_bytes, key_path):
    """Sign one catalog and its snapshot's two indexes; the bytes of its .sig file."""
    message = b''.join([
        SIGNATURE_HEADER,
        f'kind={kind}\n'.encode(),
        f'catalog={publisher.sha(catalog_bytes)}\n'.encode(),
        f'notices={publisher.sha(notices_bytes)}\n'.encode(),
        f'sources={publisher.sha(sources_bytes)}\n'.encode(),
    ])
    der = subprocess.run(['openssl', 'dgst', '-sha256', '-sign', str(key_path)], input=message,
                         capture_output=True, check=True).stdout
    return message + b'signature=' + der.hex().encode() + b'\n'


def signatures(bundles, catalogs, key_path):
    """Return the "<catalog>.sig" envelope for every catalog plan() returned."""
    envelopes = {}
    for catalog, payloads in bundles:
        key = publisher.catalog_key(catalog)
        prefix = f"snapshots/{catalog['revision']}/"
        envelopes[key + SIGNATURE_SUFFIX] = envelope(catalog['kind'], catalogs[key],
                                                     payloads[prefix + catalog['notices']],
                                                     payloads[prefix + catalog['sources']], key_path)
    return envelopes


def read_catalog(path, key):
    if not path.exists():
        return None
    value = json.loads(path.read_bytes())
    if not isinstance(value, dict) or publisher.catalog_key(value) != key:
        raise RuntimeError('existing catalog has the wrong identity: ' + key)
    return value


def plan(output, bundles):
    """Validate every bundle and return the files to write, catalogs last."""
    files, catalogs = {}, {}
    for catalog, payloads in bundles:
        publisher.validate_companions(catalog, payloads)
        key = publisher.catalog_key(catalog)
        # Keep the old checksum so readers holding the previous pack see an update.
        publisher.previous_entries(catalog, read_catalog(output / key, key))
        data = publisher.encode(catalog)
        if not 0 < len(catalog['entries']) <= CATALOG_LIMITS[catalog['kind']] or len(data) > 8192:
            raise RuntimeError(catalog['kind'] + ' catalog exceeds device limits')
        for name, body in payloads.items():
            existing = output / name
            if existing.exists() and existing.read_bytes() != body:
                raise RuntimeError('immutable snapshot collision: ' + name)
        files.update(payloads)
        catalogs[key] = data
    return files, catalogs


def write(output, files):
    for name, data in files.items():
        path = output / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--output', required=True, type=Path, help='assets/ directory of the gh-pages checkout')
    parser.add_argument('--version', required=True, help='Firmware version or release-candidate tag being released')
    parser.add_argument('--channel', required=True, choices=['stable', 'rc'],
                        help='Catalog channel the firmware reads: rc only for versions ending in -RC')
    parser.add_argument('--signing-key', required=True, type=Path,
                        help='PEM P-256 private key matching the firmware catalog key')
    args = parser.parse_args()
    if not re.fullmatch(r'[A-Za-z0-9._+-]+', args.version):
        parser.error('invalid version')
    # A catalog signed with any other key would be refused by every reader.
    if public_key_hex(args.signing_key) != firmware_public_key():
        raise RuntimeError('the signing key does not match the firmware catalog key')
    commit = subprocess.check_output(['git', '-C', str(publisher.root), 'rev-parse', 'HEAD'], text=True).strip()
    # A non-empty prepare_release marks the snapshot as a release build, as upstream's
    # release job does; its tag checks are upstream-specific and not repeated here.
    publisher.args = SimpleNamespace(channel=args.channel, release_tag=None, prepare_release=args.version)
    bundles = [publisher.build_languages(args.version, commit), publisher.build_hyphenation()]
    files, catalogs = plan(args.output, bundles)
    envelopes = signatures(bundles, catalogs, args.signing_key)
    new = sum(1 for name in files if not (args.output / name).exists())
    write(args.output, files)
    write(args.output, catalogs)
    write(args.output, envelopes)
    for key in catalogs:
        print('Catalog ' + key)
    print(f'Exported {len(files)} snapshot files ({new} new) and {len(catalogs)} catalogs to {args.output}')


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, RuntimeError, subprocess.CalledProcessError) as exc:
        sys.exit(f'Error: {exc}')
