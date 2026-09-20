#!/bin/bash
# Entrypoint of a member container: one logoscore daemon hosting
# delivery_module (+ its libp2p_module dependency), the delivery node created
# from the templated config and started. Discovery traces go to /traces.
set -u
log() { echo "[sim $(hostname)] $*"; }

IP=$(hostname -i | awk '{print $1}')
P2P_PORT=${P2P_PORT:-45000}
METRICS_PORT=${METRICS_PORT:-9100}
MESH_CONTENT_TOPIC=${MESH_CONTENT_TOPIC:-/sim/1/mesh/proto}
LOOKUP=${LOOKUP_INTERVAL:-15}
: "${SEED_ADDR:?SEED_ADDR (/ip4/../tcp/../p2p/..) is required}"

# libp2p_module defaults to /ip4/127.0.0.1/tcp/0; bind the container address so
# the records it publishes are dialable from the other containers.
export LIBP2P_MODULE_CONFIG="{\"addrs\":[\"/ip4/${IP}/tcp/${P2P_PORT}\"],\"transport\":\"tcp\",\"maxConnections\":${P2P_MAX_CONNS:-100},\"maxInConnections\":$(( ${P2P_MAX_CONNS:-100} / 2 )),\"maxOutConnections\":$(( ${P2P_MAX_CONNS:-100} / 2 ))}"
export LD_DISCO_TRACE=${LD_DISCO_TRACE:-/traces/$(hostname).trace}
mkdir -p /data "$(dirname "$LD_DISCO_TRACE")"
sed -e "s|@IP@|$IP|g" -e "s|@SEED@|$SEED_ADDR|g" -e "s|@LOOKUP@|$LOOKUP|g" \
  /opt/sim/member.json.tpl > /data/member.json

# Spread simultaneous replica starts a little: N daemons loading plugins at the
# same instant trip logos-core's 10 s "plugin never reported loading" timeout.
if [ "${START_JITTER:-0}" -gt 0 ]; then
  sleep $(( $(od -An -N2 -tu2 /dev/urandom) % START_JITTER ))
fi

log "daemon starting (ip $IP, libp2p tcp $P2P_PORT, seed $SEED_ADDR)"
logoscore daemon -m /opt/modules --persistence-path /data &
DAEMON=$!

n=0
until logoscore list-modules >/dev/null 2>&1; do
  sleep 0.2; n=$((n + 1))
  if [ "$n" -gt 300 ]; then log "daemon never answered"; kill $DAEMON; exit 1; fi
done

retry() { # <label> <cmd...>: three attempts, replies get lost under load
  # The CLI exits 0 even when the module answers with a failure, so the reply
  # body decides: a module error prints as success=false / "error" / "failed".
  local label=$1; shift; local i
  for i in 1 2 3; do
    if "$@" >/tmp/cli.out 2>&1; then
      if grep -qiE '"?success"?[[:space:]]*[:=][[:space:]]*false|^error|failed' /tmp/cli.out; then
        log "$label REFUSED: $(tr '\n' ' ' </tmp/cli.out | cut -c1-300)"
      else
        log "$label OK"; return 0
      fi
    fi
    sleep 1
  done
  log "$label FAILED: $(tr '\n' ' ' </tmp/cli.out | cut -c1-300)"; return 1
}
retry "load-module delivery_module" logoscore load-module delivery_module
retry "createNode" logoscore call delivery_module createNode @/data/member.json
retry "start" logoscore call delivery_module start

# A node joins a shard's gossipsub topic only when something subscribes: the
# startup subscription in the library runs solely for an app-supplied relay
# handler, and neither a kernel node nor the messaging client registers one.
# So ask for a content topic here. With one shard in the network every content
# topic autoshards onto shard 0, which is the mesh the members share.
retry "subscribe ${MESH_CONTENT_TOPIC}" \
  logoscore call delivery_module subscribe "${MESH_CONTENT_TOPIC}"

# /metrics for this member: the openmetrics module merges the delivery library's
# Prometheus registry (rendered text) with nim-libp2p's registry inside
# libp2p_module (structured series), labelling each series with its module.
retry "load-module openmetrics" logoscore load-module openmetrics
retry "openmetrics start" logoscore call openmetrics start \
  "{\"port\":${METRICS_PORT},\"modules\":[{\"name\":\"delivery_module\",\"format\":\"text\"},{\"name\":\"libp2p_module\",\"format\":\"data\"}]}"

wait $DAEMON
