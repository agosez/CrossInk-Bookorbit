---
title: Getting Started
parent: Development
nav_order: 1
---

# Getting Started

This guide helps you build and run CrossInk locally.

## Prerequisites

- PlatformIO Core (`pio`) or VS Code + PlatformIO IDE
- Python 3.8+
- `clang-format` 21+ in your `PATH` (CI uses clang-format 21)
- USB-C cable
- Xteink X4 or X3 device for hardware testing

If `./bin/clang-format-fix` fails with either of these errors, install clang-format 21:

- `clang-format: No such file or directory`
- `.clang-format: error: unknown key 'AlignFunctionDeclarations'`

Examples:

```sh
# Debian/Ubuntu (try this first)
sudo apt-get update && sudo apt-get install -y clang-format-21

# If the package is unavailable, add LLVM apt repo and retry
wget https://apt.llvm.org/llvm.sh
chmod +x llvm.sh
sudo ./llvm.sh 21
sudo apt-get update
sudo apt-get install -y clang-format-21

# macOS (Homebrew)
brew install clang-format
```

Then verify:

```sh
clang-format-21 --version
```

The reported major version must be 21 or newer.

## Clone and initialize

```sh
git clone --recursive https://github.com/uxjulia/CrossInk
cd CrossInk
```

If you already cloned without submodules:

```sh
git submodule update --init --recursive
```

## Build

```sh
pio run -e simulator -j1
pio run -e default -j1
```

Always select one environment and use `-j1`. EEGO A4 uses `eego_a4`
for production builds and `eego_a4-debug` for development with verbose serial
logs. Both are included in PR build validation; stable and RC workflows publish
`firmware-eego_a4-*.bin`. See [EEGO A4](./eego-a4-hardware.md) for flashing and
hardware checks.

### Font download hosting

Font downloads accept a `CROSSINK_FONT_BASE_URL` build environment
variable to use a custom host. When it is unset, font downloads use the matching
versioned GitHub release in `uxjulia/crossink-fonts`, including its individual
font files.

Use an HTTP or HTTPS URL ending in `/`. Firmware appends
`sd-fonts-m<manifest-version>-b<font-version>/fonts.json`, so point the variable
at the parent directory of those versioned folders. The catalog's `baseUrl`
should also point at your own font files.

For GitHub release and release-candidate builds, add `CROSSINK_FONT_BASE_URL`
under **Settings → Secrets and variables → Actions → Variables**. For local
builds, export the same environment variable before running PlatformIO.
The separate `CROSSINK_ASSET_BASE_URL` setting controls language and hyphenation
downloads.

HTTP remains supported for public font hosting. HTTPS font downloads use the
streaming wolfSSL transport already used by other network features, avoiding
the esp_http_client path that has stalled on ESP32-C3 devices. Downloaded font
files are validated by CRC before installation.

### Language and hyphenation download hosting

The release workflows publish language and hyphenation packs to this fork's
GitHub Pages site with `scripts/export_pages_language_assets.py`, which signs
every catalog with the `ASSET_CATALOG_SIGNING_KEY` repository secret (a PEM
P-256 private key). The firmware refuses an unsigned catalog: on the X3 and X4
these downloads use the wolfSSL transport, which does not check certificates, so
the signature, and the SHA-256 digests the catalog lists, are what authenticate
them.

To serve packs from another host, build with `CROSSINK_ASSET_BASE_URL` set to
that host and `CROSSINK_ASSET_CATALOG_KEY` set to the public half of your own
key, as an uncompressed point in lowercase hex:

```sh
openssl ecparam -name prime256v1 -genkey -noout -out catalog-key.pem
openssl ec -in catalog-key.pem -pubout -conv_form uncompressed -outform DER | tail -c 65 | xxd -p -c 65
```

Then export with `--signing-key catalog-key.pem` and the same
`CROSSINK_ASSET_CATALOG_KEY` in the environment; the script refuses a key the
firmware would not trust.

## Flash

```sh
pio run -e default --target upload
```

## Validation

```sh
./bin/clang-format-fix
pio check -e default --fail-on-defect low --fail-on-defect medium --fail-on-defect high
pio run -e default -j1
```

## What to read next

- [Architecture Overview](./architecture.md)
- [Testing and Debugging](./testing-debugging.md)
