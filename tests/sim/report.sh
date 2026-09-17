#!/usr/bin/env bash
# Discovery report over the member traces of the running/last compose run.
set -euo pipefail
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
traces=$here/out/traces
n=$(ls "$traces"/*.trace 2>/dev/null | wc -l | tr -d ' ')
[ "$n" -gt 0 ] || { echo "no traces in $traces" >&2; exit 1; }
# expected ids in the DHT = members + the seed
python3 "$here/disco_report.py" "$traces" $((n + 1))
