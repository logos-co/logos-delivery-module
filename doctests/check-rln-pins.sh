#!/usr/bin/env bash
#
# Assert that the RLN dependency revs pinned in the runtime doc-test match
# flake.lock. The doc-test runner executes every step in a scratch directory
# with no access to this repo, so the spec has to name the revs literally
# instead of resolving them from the lock; this keeps the two in step.
#
# Run from anywhere; bump the revs in delivery-module-runtime.test.yaml when
# this fails.
set -euo pipefail

cd "$(dirname "$0")/.."

SPEC="doctests/delivery-module-runtime.test.yaml"

rln=$(jq -r '.nodes[.root].inputs.liblogos_rln_module' flake.lock)
lez=$(jq -r --arg n "$rln" '.nodes[$n].inputs.liblogos_lez_rln_module' flake.lock)
core=$(jq -r --arg n "$lez" '.nodes[$n].inputs.lez_core' flake.lock)

status=0
for node in "$rln" "$lez" "$core"; do
  rev=$(jq -r --arg n "$node" '.nodes[$n].locked.rev' flake.lock)
  if ! grep -q "$rev" "$SPEC"; then
    echo "$SPEC does not pin $node at $rev (flake.lock)" >&2
    status=1
  fi
done

if [ "$status" -ne 0 ]; then
  echo "Update the 'Build the RLN dependency chain' step to the locked revs." >&2
  exit 1
fi

echo "RLN pins in $SPEC match flake.lock"
