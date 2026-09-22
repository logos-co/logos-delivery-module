#!/usr/bin/env bash
# Run the RLN-off portion of delivery-module-runtime.test.yaml on native Windows.
# The shared Windows CI action invokes this from the staged tree root.
set -euo pipefail

cli=windows-logoscore/bin/logoscore.exe
config_dir=./windows-smoke-config
node_config=windows-smoke-node.json
module_dir=windows-logoscore/modules/delivery_module

require_output() {
  local label=$1 output=$2 expected=$3
  if [[ "$output" != *"$expected"* ]]; then
    printf '%s: expected %s in output:\n%s\n' "$label" "$expected" "$output" >&2
    exit 1
  fi
  printf '%s: passed\n' "$label"
}

test ! -e "$module_dir"
cp -R install-portable/modules/delivery_module "$module_dir"
test -f "$module_dir/manifest.json"
printf '{"logLevel":"INFO"}\n' > "$node_config"

"./$cli" -D -m ./windows-logoscore/modules --config-dir "$config_dir" \
  > windows-smoke-daemon.log 2>&1 &

cleanup() {
  run "$cli" --config-dir "$config_dir" stop >/dev/null 2>&1 || true
}
trap cleanup EXIT

ready=0
for _ in $(seq 1 20); do
  if status=$(run "$cli" --config-dir "$config_dir" status 2>/dev/null) &&
     [[ "$status" == *'"status":"running"'* ]]; then
    ready=1
    break
  fi
  sleep 1
done
if [ "$ready" -ne 1 ]; then
  cat windows-smoke-daemon.log >&2
  exit 1
fi
printf 'daemon: ready\n'

output=$(run "$cli" --config-dir "$config_dir" list-modules)
require_output 'discover delivery_module' "$output" '"name":"delivery_module"'

output=$(run "$cli" --config-dir "$config_dir" load-module delivery_module)
require_output 'load delivery_module' "$output" '"status":"ok"'
require_output 'skip optional RLN module' "$output" '"module":"liblogos_rln_module"'
require_output 'RLN module is absent' "$output" '"reason":"not_installed"'

output=$(run "$cli" --config-dir "$config_dir" module-info delivery_module)
require_output 'inspect delivery_module' "$output" '"name":"delivery_module"'
require_output 'find createNode' "$output" '"name":"createNode"'

output=$(run "$cli" --config-dir "$config_dir" call delivery_module createNode "@$node_config")
require_output 'createNode' "$output" '"success":true'

output=$(run "$cli" --config-dir "$config_dir" call delivery_module rlnState)
require_output 'RLN stays disabled' "$output" '"state":"Disabled"'

output=$(run "$cli" --config-dir "$config_dir" stop)
require_output 'stop daemon' "$output" '"status":"ok"'
trap - EXIT
