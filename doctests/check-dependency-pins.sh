#!/usr/bin/env bash
#
# Assert that the dependency revs (libp2p_module and the RLN chain) pinned in
# the runtime doc-test match flake.lock. The doc-test runner executes every step in a scratch directory
# with no access to this repo, so the spec has to name the revs literally
# instead of resolving them from the lock; this keeps the two in step.
#
# Run from anywhere; bump the revs in delivery-module-runtime.test.yaml when
# this fails.
set -euo pipefail

cd "$(dirname "$0")/.."

SPEC="doctests/delivery-module-runtime.test.yaml"

status=0
for input in libp2p_module rln_module rln_lez_module membership_allocation_client_module; do
  node=$(jq -r --arg i "$input" '.nodes[.root].inputs[$i] // empty' flake.lock)
  if [ -z "$node" ]; then
    echo "flake.lock has no top-level input $input (relock)" >&2
    status=1
    continue
  fi
  rev=$(jq -r --arg n "$node" '.nodes[$n].locked.rev' flake.lock)
  if ! grep -q "$rev" "$SPEC"; then
    echo "$SPEC does not pin $input at $rev (flake.lock)" >&2
    status=1
  fi
done

if [ "$status" -ne 0 ]; then
  echo "Update the 'Build the libp2p dependency' and 'Build the RLN dependency chain' steps to the locked revs." >&2
  exit 1
fi

echo "Dependency pins in $SPEC match flake.lock"
