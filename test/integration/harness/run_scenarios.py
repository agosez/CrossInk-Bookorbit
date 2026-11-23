#!/usr/bin/env python3
"""Run BookOrbit integration scenarios against the simulator.

Scenarios live in scenarios/, one module per functional area. Each gets an isolated SD
card (sim.py), pre-written with BookOrbit credentials, settings and the books it needs;
it drives the simulator headless, then asserts on the card (formats.py) and on the
server through its API (server.py).

Prerequisites: `docker compose -f ../docker-compose.yml up -d`, `seed/seed.py`
ran green, and `pio run -e simulator` built the binary.
"""

from __future__ import annotations

import argparse
import subprocess
import sys
import textwrap
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "seed"))  # kosync

import sim  # noqa: E402
from scenarios import AREAS, collect  # noqa: E402


def print_inventory(scenarios: dict) -> None:
    """Every scenario by area, with the first paragraph of what it proves."""
    for area in AREAS:
        print(f"{area}:")
        for name in sorted(n for n, (a, _) in scenarios.items() if a == area):
            summary = " ".join((scenarios[name][1].__doc__ or "").strip().split("\n\n")[0].split())
            print(f"  {name}")
            print(textwrap.fill(summary, width=100, initial_indent="      ", subsequent_indent="      "))


def main() -> int:
    scenarios = collect()
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--scenario", action="append", choices=sorted(scenarios), metavar="NAME",
                        help="run this scenario (repeatable)")
    parser.add_argument("--area", action="append", choices=AREAS, help="run this area's scenarios (repeatable)")
    parser.add_argument("--list", action="store_true", help="list the scenarios by area and exit")
    parser.add_argument("-v", "--verbose", action="store_true", help="print simulator output")
    args = parser.parse_args()

    if args.list:
        print_inventory(scenarios)
        return 0
    sim.VERBOSE = args.verbose

    # A previous aborted run can leave an orphaned simulator alive, holding
    # stale state against the same server.
    subprocess.run(["pkill", "-9", "-f", str(sim.PROGRAM)], check=False)

    if not sim.PROGRAM.exists():
        print(f"Simulator binary missing: {sim.PROGRAM}\nRun: pio run -e simulator", file=sys.stderr)
        return 2

    names = sorted(name for name, (area, _) in scenarios.items()
                   if (not args.scenario and not args.area)
                   or name in (args.scenario or ()) or area in (args.area or ()))
    failures = 0
    for name in names:
        start = time.monotonic()
        try:
            scenarios[name][1]()
            print(f"PASS {name} ({time.monotonic() - start:.1f}s)")
        except AssertionError as e:
            failures += 1
            print(f"FAIL {name}: {e}", file=sys.stderr)
        except Exception as e:  # infrastructure error, not a test verdict
            failures += 1
            print(f"ERROR {name}: {type(e).__name__}: {e}", file=sys.stderr)
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
