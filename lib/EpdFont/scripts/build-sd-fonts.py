#!/usr/bin/env python3
"""Build SD card fonts from a declarative YAML config.

Reads sd-fonts.yaml, downloads any missing source fonts, runs
fontconvert_sdcard.py in parallel for each family, and optionally
generates the fonts.json manifest.

Usage:
    # Generate fonts + per-family ZIPs in ../crossink-fonts/cpfonts
    python3 build-sd-fonts.py

    # Generate fonts without ZIP packaging
    python3 build-sd-fonts.py --no-package

    # Generate fonts + manifest
    python3 build-sd-fonts.py --manifest --base-url "http://localhost:8000/"

    # Custom config / output paths
    python3 build-sd-fonts.py --config my-fonts.yaml --output-dir dist/

    # Generate only specific families
    python3 build-sd-fonts.py --only Literata,IBMPlexMono

    # Stream child process output for debugging
    python3 build-sd-fonts.py --verbose

    # Override the per-family timeout (default: 600s)
    python3 build-sd-fonts.py --timeout 1200
"""

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import socket
import urllib.request
from concurrent.futures import ProcessPoolExecutor, as_completed
from pathlib import Path

import yaml

SCRIPT_DIR = Path(__file__).resolve().parent
FONTCONVERT = SCRIPT_DIR / "fontconvert_sdcard.py"
EPDFONTS_DIR = SCRIPT_DIR.parent  # lib/EpdFont
PROJECT_ROOT = SCRIPT_DIR.parents[2]
DEFAULT_CONFIG = SCRIPT_DIR / "sd-fonts.yaml"
DEFAULT_OUTPUT = PROJECT_ROOT.parent / "crossink-fonts" / "cpfonts"
DOWNLOAD_DIR = SCRIPT_DIR / "downloaded_fonts"
INSTANCE_DIR = SCRIPT_DIR / "instanced_fonts"
DEFAULT_FALLBACK_FONT = EPDFONTS_DIR / "builtinFonts/source/NotoSans/NotoSans-Regular.ttf"
# Every generated SD-card reader font should cover the same core glyph ranges
# as the built-in reader fonts. Families can still add extra script presets.
PATCHED_INTERVAL_PRESETS = ("builtin",)

# Keep the SD-card fallback stack aligned with the built-in non-emoji ranges.
# SD-card fonts deliberately retain the extra emoji fallback because they do
# not consume firmware space. Noto Sans remains the final unrestricted
# fallback for missing glyphs, including the IPA ranges requested by dictionary
# fonts.
COMMON_FALLBACK_RANGES = (
    (0x03BB, 0x03BB),
    (0x0410, 0x0414), (0x0418, 0x0418), (0x041B, 0x041B),
    (0x041D, 0x0423), (0x0425, 0x0425), (0x0427, 0x0427),
    (0x042B, 0x042C), (0x042E, 0x0432), (0x0434, 0x0435),
    (0x0437, 0x0437), (0x043A, 0x043A), (0x043D, 0x043E),
    (0x0440, 0x0440), (0x0442, 0x0442), (0x0446, 0x0446),
    (0x044C, 0x044C), (0x044E, 0x044E), (0x2113, 0x2113),
)
EMOJI_FALLBACK_RANGES = (
    (0x2669, 0x266F),
    (0x1F600, 0x1F607), (0x1F609, 0x1F614), (0x1F618, 0x1F618),
    (0x1F61A, 0x1F61A), (0x1F61C, 0x1F61D), (0x1F620, 0x1F622),
    (0x1F624, 0x1F625), (0x1F629, 0x1F629), (0x1F62C, 0x1F62E),
    (0x1F631, 0x1F635), (0x1F641, 0x1F642), (0x1F644, 0x1F644),
    (0x1F44B, 0x1F44F), (0x2764, 0x2764),
)
SYMBOL_FALLBACK_RANGES = (
    (0x2191, 0x2191),  # upward arrow used in dictionary pronunciation guides
    (0x2669, 0x266F),
)
PHM_FALLBACK_RANGES = (
    (0x4F1A, 0x4F1A), (0x53BB, 0x53BB), (0x5458, 0x5458),
    (0x59DA, 0x59DA), (0x5B98, 0x5B98), (0x5BA4, 0x5BA4),
    (0x5E26, 0x5E26), (0x6211, 0x6211), (0x62C9, 0x62C9),
    (0x653E, 0x653E), (0x6746, 0x677F), (0x7532, 0x7532),
    (0x7684, 0x7684), (0x8BAE, 0x8BAE), (0x8BF7, 0x8BF7),
    (0x91CA, 0x91CA),
)
PATCHED_INTERVAL_RANGES = (
    *COMMON_FALLBACK_RANGES,
    *EMOJI_FALLBACK_RANGES,
    *SYMBOL_FALLBACK_RANGES,
    *PHM_FALLBACK_RANGES,
)
STYLE_SUFFIXES = {
    "regular": "Regular",
    "bold": "Bold",
    "italic": "Italic",
    "bolditalic": "BoldItalic",
}


def is_url(value: str) -> bool:
    return value.startswith(("http://", "https://"))


def validate_config(families: list[dict]) -> list[str]:
    """Return human-readable config errors."""
    errors: list[str] = []
    for family in families:
        family_name = family.get("name", "<unnamed>")
        for style_name, style_spec in family.get("styles", {}).items():
            source_keys = [key for key in ("path", "url") if key in style_spec]
            if len(source_keys) != 1:
                errors.append(
                    f"{family_name}/{style_name}: use exactly one of 'path' or 'url'"
                )
                continue

            if "path" in style_spec and is_url(str(style_spec["path"])):
                errors.append(
                    f"{family_name}/{style_name}: URL was placed under 'path'; "
                    "use 'url' for downloadable fonts"
                )

    return errors


def patched_intervals(intervals: str) -> str:
    """Append required fallback-backed intervals to a family interval list."""
    requested = [part.strip() for part in intervals.split(",") if part.strip()]
    normalized = {part.lower() for part in requested}
    patched = list(requested)

    for preset in PATCHED_INTERVAL_PRESETS:
        if preset not in normalized:
            patched.append(preset)

    for start, end in PATCHED_INTERVAL_RANGES:
        interval = f"(0x{start:04X}-0x{end:04X})"
        if interval.lower() not in normalized:
            patched.append(interval)

    return ",".join(patched)


def builtin_fallback_specs(style_name: str, family_name: str) -> list[tuple[Path, tuple | None]]:
    """Return the built-in fallback faces and their restricted ranges."""
    suffix = STYLE_SUFFIXES[style_name]
    specs = []

    # ChareInk is the common fallback used by the built-in reader families.
    # Do not add it to ChareInk itself; its primary face is already that font.
    if family_name.lower() != "chareink":
        specs.append((
            EPDFONTS_DIR / f"builtinFonts/source/ChareInk7/ChareInk7-{suffix}.ttf",
            COMMON_FALLBACK_RANGES,
        ))

    # SD-card fonts retain the regular emoji face for every style.
    specs.append((
        EPDFONTS_DIR / "builtinFonts/source/NotoEmoji/NotoEmoji-Regular.ttf",
        EMOJI_FALLBACK_RANGES,
    ))
    specs.append((
        EPDFONTS_DIR / "builtinFonts/source/NotoSymbols/NotoSansSymbols-Regular.ttf",
        SYMBOL_FALLBACK_RANGES,
    ))

    # PHM CJK fallback is intentionally only part of regular reader fonts.
    if style_name == "regular":
        specs.append((
            EPDFONTS_DIR / "builtinFonts/source/NotoSansCJKsc/NotoSansCJKsc-Regular.otf",
            PHM_FALLBACK_RANGES,
        ))

    # Keep the existing SD-font behavior for any requested glyph not supplied
    # by the built-in fallback stack, including dictionary IPA coverage.
    specs.append((DEFAULT_FALLBACK_FONT, None))
    return specs


def encode_fallback_ranges(ranges: tuple | None) -> str:
    """Encode fallback ranges for fontconvert_sdcard.py's CLI."""
    if not ranges:
        return ""
    return ";".join(f"0x{start:X}-0x{end:X}" for start, end in ranges)


def append_fallback_args(cmd: list[str], style_name: str, family_name: str) -> None:
    """Append one ordered fallback stack for a style."""
    range_flag = f"--fallback-{style_name}-ranges"
    font_flag = f"--fallback-{style_name}"
    for fallback_path, ranges in builtin_fallback_specs(style_name, family_name):
        cmd.extend([font_flag, str(fallback_path), range_flag, encode_fallback_ranges(ranges)])


_orig_getaddrinfo = socket.getaddrinfo


def _ipv4_only_getaddrinfo(*args, **kwargs):
    """getaddrinfo variant that drops AAAA records (IPv4 only)."""
    return [ai for ai in _orig_getaddrinfo(*args, **kwargs) if ai[0] == socket.AF_INET]


def download_font(url: str, dest: Path, retries: int = 3) -> Path:
    """Download a font file if not already cached. Returns the local path.

    Some sources (e.g. mirrors.ctan.org) are round-robin redirectors that land
    on a different mirror each request; a mirror may advertise an IPv6 address a
    host without an IPv6 route cannot reach ([Errno 101] Network is unreachable).
    Retry on failure, forcing IPv4 resolution after the first attempt.
    """
    if dest.exists():
        return dest
    dest.parent.mkdir(parents=True, exist_ok=True)
    print(f"  Downloading {dest.name}...")
    last_err = None
    for attempt in range(1, retries + 1):
        force_ipv4 = attempt > 1
        if force_ipv4:
            socket.getaddrinfo = _ipv4_only_getaddrinfo
        try:
            urllib.request.urlretrieve(url, dest)
            break
        except Exception as e:  # noqa: BLE001 - reported via RuntimeError below
            last_err = e
            dest.unlink(missing_ok=True)
            if attempt < retries:
                print(f"  Attempt {attempt} failed ({e}); retrying (IPv4-only)...")
        finally:
            if force_ipv4:
                socket.getaddrinfo = _orig_getaddrinfo
    else:
        raise RuntimeError(f"Failed to download {url}: {last_err}") from last_err
    size_kb = dest.stat().st_size / 1024
    print(f"  Downloaded {dest.name} ({size_kb:.0f} KB)")
    return dest


def extract_static_instance(source_path: Path, axes: dict, family_name: str, style_name: str) -> Path:
    """Use fonttools instancer to pin variable font axes, producing a static TTF.

    Caches the result in INSTANCE_DIR/<family>/<style>_<axes>_<mtime>.ttf.
    Returns the path to the static font file.
    """
    from fontTools.varLib.instancer import instantiateVariableFont
    from fontTools.ttLib import TTFont

    mtime = int(source_path.stat().st_mtime)
    axis_key = "_".join(f"{k}{v}" for k, v in sorted(axes.items()))
    cache_name = f"{style_name}_{axis_key}_{mtime}.ttf"
    cached = INSTANCE_DIR / family_name / cache_name

    if cached.exists():
        return cached

    # Clean old cached instances for this style
    cached.parent.mkdir(parents=True, exist_ok=True)
    for old in cached.parent.glob(f"{style_name}_*.ttf"):
        old.unlink()

    print(f"  Extracting static instance: {family_name}/{style_name} ({axis_key})")
    # Atomic write: save to a temp file first, then rename. A crash or save()
    # exception would otherwise leave a corrupt `cached` file that future runs
    # would happily reuse via the `cached.exists()` check above.
    tmp_fd, tmp_name = tempfile.mkstemp(suffix=".ttf", dir=cached.parent)
    os.close(tmp_fd)
    tmp_path = Path(tmp_name)
    # Keep separate handles for the source variable font and the static
    # instance: instantiateVariableFont with default inplace=False returns a
    # *new* TTFont, so rebinding `font` would otherwise strand the source's
    # file handle open until GC runs.
    #
    # updateFontNames=True   — rewrite the name table so the saved font
    #                          reports its weight/style accurately rather
    #                          than retaining the variable-font names.
    # optimize=False         — skip the gvar interpolation optimisation;
    #                          fully pinning every axis drops gvar anyway,
    #                          so the work would be wasted.
    source_font = TTFont(str(source_path))
    try:
        font = instantiateVariableFont(source_font, axes, updateFontNames=True, optimize=False)
        try:
            font.save(str(tmp_path))
        finally:
            font.close()
    except Exception:
        tmp_path.unlink(missing_ok=True)
        raise
    finally:
        source_font.close()
    tmp_path.replace(cached)

    return cached


def resolve_font_path(style_spec: dict, family_name: str, style_name: str) -> Path:
    """Resolve a style spec (path or url) to a local font file path.

    If 'variable' key is present, extracts a static instance via fonttools
    instancer after resolving the source file.
    """
    if "path" in style_spec:
        resolved = EPDFONTS_DIR / style_spec["path"]
        if not resolved.exists():
            raise FileNotFoundError(f"{family_name}/{style_name}: {resolved} not found")
    elif "url" in style_spec:
        url = style_spec["url"]
        # Derive a stable filename from the URL
        filename = url.rsplit("/", 1)[-1]
        dest = DOWNLOAD_DIR / family_name / filename
        resolved = download_font(url, dest)
    else:
        raise ValueError(f"{family_name}/{style_name}: must have 'path' or 'url'")

    # If variable font axes are specified, extract a static instance
    if "variable" in style_spec:
        resolved = extract_static_instance(
            resolved, style_spec["variable"], family_name, style_name
        )

    return resolved


def _stream_pipe(pipe, prefix: str, dest: list[str]):
    """Read lines from a pipe, print with prefix, and accumulate into dest."""
    for line in pipe:
        dest.append(line)
        print(f"  [{prefix}] {line}", end="", flush=True)


def build_family(
    family: dict, output_base: Path, verbose: bool = False, timeout: int = 600
) -> tuple[str, bool, str]:
    """Build a single font family. Returns (name, success, message)."""
    name = family["name"]
    output_dir = output_base / name
    output_dir.mkdir(parents=True, exist_ok=True)

    styles = family.get("styles", {})
    intervals = patched_intervals(str(family["intervals"]))
    sizes = ",".join(str(s) for s in family["sizes"])

    # Resolve all font file paths (downloads as needed)
    try:
        resolved_styles = {}
        for style_name, style_spec in styles.items():
            resolved_styles[style_name] = resolve_font_path(style_spec, name, style_name)
    except (FileNotFoundError, RuntimeError) as e:
        return name, False, str(e)

    # Build the fontconvert_sdcard.py command
    cmd = [sys.executable, str(FONTCONVERT)]

    multi_style = len(resolved_styles) > 1 or "regular" not in resolved_styles
    has_any_multi = any(k in resolved_styles for k in ("regular", "bold", "italic", "bolditalic"))

    if has_any_multi and len(resolved_styles) > 1:
        # Multi-style mode
        for style_name, font_path in resolved_styles.items():
            cmd.extend([f"--{style_name}", str(font_path)])
            append_fallback_args(cmd, style_name, name)
    else:
        # Single-style mode
        style_name = next(iter(resolved_styles))
        font_path = resolved_styles[style_name]
        cmd.append(str(font_path))
        cmd.extend(["--style", style_name])
        append_fallback_args(cmd, style_name, name)

    cmd.extend(["--intervals", intervals])
    cmd.extend(["--sizes", sizes])
    cmd.extend(["--name", name])
    cmd.extend(["--output-dir", str(output_dir) + "/"])

    if family.get("force_autohint", False):
        cmd.append("--force-autohint")

    # SD-card fonts are reader fonts, so they get the same darkened anti-alias
    # thresholds as the built-in reader fonts in convert-builtin-fonts.sh
    # (READING_FONT_RENDER_ARGS). Without this the two look noticeably
    # different at the same size on the same panel.
    cmd.append("--darken-aa")

    # Run fontconvert_sdcard.py
    start = time.monotonic()
    try:
        if verbose:
            proc = subprocess.Popen(
                cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True
            )
            stdout_lines: list[str] = []
            stderr_lines: list[str] = []
            t_out = threading.Thread(
                target=_stream_pipe, args=(proc.stdout, name, stdout_lines)
            )
            t_err = threading.Thread(
                target=_stream_pipe, args=(proc.stderr, f"{name}/err", stderr_lines)
            )
            t_out.start()
            t_err.start()
            try:
                proc.wait(timeout=timeout)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
                elapsed = time.monotonic() - start
                return name, False, f"Timed out after {elapsed:.0f}s"
            finally:
                t_out.join()
                t_err.join()

            if proc.returncode != 0:
                err = "".join(stderr_lines).strip()
                return name, False, err or f"Exit code {proc.returncode}"
            return name, True, ""
        else:
            result = subprocess.run(
                cmd, capture_output=True, text=True, timeout=timeout,
            )
            if result.returncode != 0:
                return name, False, result.stderr.strip() or f"Exit code {result.returncode}"
            return name, True, ""
    except subprocess.TimeoutExpired as e:
        elapsed = time.monotonic() - start
        tail = ""
        captured = getattr(e, "stderr", None) or getattr(e, "stdout", None)
        if captured:
            lines = captured.strip().splitlines()
            tail = "\n    Last output:\n" + "\n".join(f"    | {l}" for l in lines[-20:])
        return name, False, f"Timed out after {elapsed:.0f}s{tail}"
    except Exception as e:
        return name, False, str(e)


def package_family(output_base: Path, family: dict) -> Path:
    """Create ``<Family>.zip`` containing the matching ``<Family>/`` folder."""
    family_name = family["name"]
    family_dir = output_base / family_name
    if not family_dir.is_dir():
        raise FileNotFoundError(f"Expected generated family directory: {family_dir}")

    expected_files = len(family.get("sizes", []))
    generated_files = list(family_dir.glob("*.cpfont"))
    if len(generated_files) != expected_files:
        raise RuntimeError(
            f"{family_name}: expected {expected_files} .cpfont files, "
            f"found {len(generated_files)}; rerun with --clean to remove stale output"
        )

    archive_base = output_base / family_name
    return Path(
        shutil.make_archive(
            str(archive_base), "zip", root_dir=str(output_base), base_dir=family_name
        )
    )


def generate_manifest(
    config_path: Path, output_base: Path, base_url: str, manifest_path: Path
):
    """Generate fonts.json manifest from config + built output.

    Uses the standalone generate-font-manifest.py as a subprocess so
    display metadata comes from the YAML config via --descriptions-from.
    """
    manifest_script = SCRIPT_DIR.parent.parent.parent / "scripts" / "generate-font-manifest.py"

    if not base_url.endswith("/"):
        base_url += "/"

    cmd = [
        sys.executable, str(manifest_script),
        "--input", str(output_base),
        "--base-url", base_url,
        "--output", str(manifest_path),
    ]

    if config_path.exists():
        cmd.extend(["--descriptions-from", str(config_path)])

    manifest_path.parent.mkdir(parents=True, exist_ok=True)
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        print(f"ERROR: Manifest generation failed:\n{result.stderr}", file=sys.stderr)
        return
    print(result.stdout, end="")
    print(f"Manifest written: {manifest_path}")


def main():
    parser = argparse.ArgumentParser(description="Build SD card fonts from YAML config")
    parser.add_argument(
        "--config", default=str(DEFAULT_CONFIG), help="Path to font families YAML config"
    )
    parser.add_argument(
        "--output-dir",
        default=str(DEFAULT_OUTPUT),
        help="Output directory for family folders and ZIPs (default: sibling crossink-fonts/cpfonts)",
    )
    parser.add_argument("--only", help="Comma-separated family names to build (default: all)")
    parser.add_argument("--manifest", action="store_true", help="Also generate fonts.json manifest")
    parser.add_argument("--base-url", default="", help="Base URL for manifest (required with --manifest)")
    parser.add_argument(
        "--manifest-output", default=None, help="Manifest output path (default: <output-dir>/fonts.json)"
    )
    parser.add_argument(
        "--jobs", "-j", type=int, default=None,
        help="Max parallel jobs (default: number of families)"
    )
    parser.add_argument("--clean", action="store_true", help="Clean output directory before building")
    parser.add_argument(
        "--verbose", "-v", action="store_true",
        help="Stream child process output in real time (useful for debugging timeouts)"
    )
    parser.add_argument(
        "--timeout", type=int, default=600,
        help="Per-family timeout in seconds (default: 600)"
    )
    parser.add_argument(
        "--no-package",
        action="store_true",
        help="Skip per-family ZIP packaging",
    )
    args = parser.parse_args()

    if args.manifest and not args.base_url:
        parser.error("--base-url is required when using --manifest")

    # Load config
    config_path = Path(args.config)
    if not config_path.exists():
        print(f"ERROR: Config not found: {config_path}", file=sys.stderr)
        sys.exit(1)

    with open(config_path) as f:
        config = yaml.safe_load(f)

    families = config.get("families", [])
    if not families:
        print("ERROR: No families defined in config", file=sys.stderr)
        sys.exit(1)

    if not DEFAULT_FALLBACK_FONT.exists() or not DEFAULT_FALLBACK_FONT.is_file():
        print(
            "ERROR: Missing default fallback font: "
            f"{DEFAULT_FALLBACK_FONT}\n"
            "This font is required for fallback glyphs in SD font builds.",
            file=sys.stderr,
        )
        sys.exit(1)

    fallback_paths = {
        fallback_path
        for family in families
        for style_name in family.get("styles", {})
        for fallback_path, _ in builtin_fallback_specs(style_name, family["name"])
    }
    missing_fallbacks = sorted(path for path in fallback_paths if not path.is_file())
    if missing_fallbacks:
        print("ERROR: Missing built-in fallback fonts:", file=sys.stderr)
        for fallback_path in missing_fallbacks:
            print(f"  {fallback_path}", file=sys.stderr)
        sys.exit(1)

    # Filter if --only specified
    if args.only:
        only_names = set(args.only.split(","))
        families = [f for f in families if f["name"] in only_names]
        missing = only_names - {f["name"] for f in families}
        if missing:
            print(f"WARNING: families not found in config: {', '.join(missing)}", file=sys.stderr)
        if not families:
            print("ERROR: no matching families after --only filter", file=sys.stderr)
            sys.exit(1)

    config_errors = validate_config(families)
    if config_errors:
        print("ERROR: invalid font config:", file=sys.stderr)
        for error in config_errors:
            print(f"  - {error}", file=sys.stderr)
        sys.exit(1)

    output_base = Path(args.output_dir)

    if args.clean and output_base.exists():
        print(f"Cleaning {output_base}...")
        shutil.rmtree(output_base)

    output_base.mkdir(parents=True, exist_ok=True)

    # Download phase (sequential — avoids hammering servers)
    print(f"\n=== Resolving {len(families)} font families ===\n")
    for family in families:
        for style_name, style_spec in family.get("styles", {}).items():
            if "url" in style_spec:
                try:
                    resolve_font_path(style_spec, family["name"], style_name)
                except Exception as e:
                    print(f"ERROR: {e}", file=sys.stderr)
                    sys.exit(1)

    # Build phase (parallel)
    max_workers = args.jobs or len(families)
    verbose = args.verbose
    timeout = args.timeout
    print(f"\n=== Building {len(families)} families ({max_workers} parallel jobs, timeout {timeout}s) ===\n")

    failed = []
    with ProcessPoolExecutor(max_workers=max_workers) as executor:
        futures = {
            executor.submit(build_family, family, output_base, verbose, timeout): family["name"]
            for family in families
        }
        for future in as_completed(futures):
            name, success, message = future.result()
            if success:
                # Count output files
                family_dir = output_base / name
                count = len(list(family_dir.glob("*.cpfont")))
                size = sum(f.stat().st_size for f in family_dir.glob("*.cpfont"))
                print(f"  OK: {name} ({count} files, {size / 1024 / 1024:.1f} MB)")
            else:
                print(f"  FAILED: {name}: {message}", file=sys.stderr)
                failed.append(name)

    # Summary
    print("\n=== Summary ===\n")
    total_files = len(list(output_base.rglob("*.cpfont")))
    total_size = sum(f.stat().st_size for f in output_base.rglob("*.cpfont"))
    print(f"Total: {total_files} .cpfont files ({total_size / 1024 / 1024:.1f} MB)")

    if failed:
        print(f"\nFailed families: {', '.join(failed)}", file=sys.stderr)

    # Package only a completely successful build. This keeps an old ZIP from
    # looking current when one family failed partway through conversion.
    if not failed and not args.no_package:
        print("\n=== Packaging SD-card font ZIPs ===\n")
        packaged_archives = []
        try:
            for family in families:
                archive = package_family(output_base, family)
                packaged_archives.append(archive)
                print(f"  ZIP: {archive}")
        except (FileNotFoundError, OSError, RuntimeError) as error:
            print(f"ERROR: packaging failed: {error}", file=sys.stderr)
            sys.exit(1)
        print(
            f"\nPackaged {total_files} .cpfont files and {len(packaged_archives)} ZIPs "
            f"in {output_base}"
        )

    # Manifest
    if args.manifest:
        manifest_path = Path(args.manifest_output) if args.manifest_output else output_base / "fonts.json"
        generate_manifest(config_path, output_base, args.base_url, manifest_path)

    if failed:
        sys.exit(1)


if __name__ == "__main__":
    main()
