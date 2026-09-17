#!/usr/bin/env bash
# up.sh <members> [subnet]: generate the member services and start the stack.
set -euo pipefail
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
n=${1:?usage: up.sh <members> [subnet]}
subnet=${2:-${SUBNET:-10.0.0.0/8}}
shift $(( $# > 1 ? 2 : 1 ))
mkdir -p "$here/out/traces"
python3 "$here/gen-members.py" "$n" "$subnet" --targets "$here/out/prom-targets.json" \
  > "$here/out/members.yml"
SUBNET=$subnet docker compose -f "$here/docker-compose.yml" -f "$here/out/members.yml" up -d "$@"
