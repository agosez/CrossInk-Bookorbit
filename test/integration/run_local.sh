#!/usr/bin/env sh
# Run the whole BookOrbit integration suite locally: server up, seed, simulator
# build, scenarios. Suitable as a pre-push hook:
#
#   ln -s ../../test/integration/run_local.sh .git/hooks/pre-push
#
# Environment: PIO overrides the PlatformIO executable (e.g. a venv's bin/pio);
# docker is also looked up under ~/.docker/bin (Docker Desktop on macOS does
# not put it on PATH).
set -e
cd "$(dirname "$0")/../.."

command -v docker >/dev/null 2>&1 || PATH="$HOME/.docker/bin:$PATH"
PIO="${PIO:-pio}"

docker compose -f test/integration/docker-compose.yml up -d --wait

# Idempotent: regenerates nothing when the library exists, reseeds nothing the
# server already holds.
[ -f test/integration/library.json ] || python3 test/integration/seed/make_library.py
python3 test/integration/seed/seed.py

"$PIO" run -e simulator
python3 test/integration/harness/run_scenarios.py
