#!/bin/sh
# Build and run a delivery node under the headless logos-core runtime.
#
# Wraps the whole sequence from docs/run-node.md — build logoscore + lgpm, build
# this checkout's .lgx, install it next to capability_module, start the daemon,
# load the modules, createNode, start — behind one command:
#
#   scripts/run-node.sh up                      # boot with conf/logos-test.json
#   scripts/run-node.sh up -c conf/logos-dev.json
#   scripts/run-node.sh status
#   scripts/run-node.sh call getNodeInfo listenAddresses
#   scripts/run-node.sh down
#
# Everything it produces lives under the work dir (default .run/), including the
# node's persistence, so `down --clean` leaves no trace. The logoscore daemon is
# a singleton per user — its connection file is ~/.logoscore/daemon.json — so
# only one node at a time, whoever started it.
#
# libp2p_module is always installed here: on this branch delivery_module lists
# it in metadata.json#dependencies, so logos-core auto-loads it whenever
# delivery_module is loaded — this script never loads it explicitly. (On the
# sibling branch poc-apply-discovery-plugin it is undeclared and installed only
# when the config asks for plugin-hosted kademlia discovery.)
set -eu

REPO=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

# Sources for the pieces this repo does not build itself. Override to build from
# a local checkout, e.g. LIBP2P_FLAKE=/path/to/logos-libp2p-module#lgx
LOGOSCORE_FLAKE=${LOGOSCORE_FLAKE:-github:logos-co/logos-logoscore-cli}
LGPM_FLAKE=${LGPM_FLAKE:-github:logos-co/logos-package-manager#cli}
LIBP2P_FLAKE=${LIBP2P_FLAKE:-github:logos-co/logos-libp2p-module#lgx}

CONFIG=$REPO/conf/logos-test.json
WORKDIR=$REPO/.run
WANT_LIBP2P=auto
REBUILD=0
CLEAN=0
ACTION=

usage() {
  cat <<EOF
Usage: $(basename "$0") <command> [options]

Commands:
  up                 build if needed, boot the daemon, create and start the node
  down               stop the node and the daemon
  status             daemon and module health
  call <method> [args...]
                     call a delivery_module method on the running node
  logs               tail the daemon log

Options:
  -c, --config FILE  node config passed to createNode (default: conf/logos-test.json)
  -w, --workdir DIR  where artifacts, modules and node data live (default: .run)
      --libp2p       install libp2p_module (the default; it is a dependency)
      --no-libp2p    leave it out, to see the missing-dependency failure
      --rebuild      rebuild the nix outputs even if they are already linked
      --clean        (with down) also delete the work dir
  -h, --help         this text

Environment:
  LD_DISCO_TRACE     file to trace every service-discovery verb into. logos-core
                     discards a module's stdout, so this is the only way to
                     watch the plugin boundary in a running node.
  LIBP2P_MODULE_CONFIG
                     JSON overlaid on libp2p_module's options when it loads
                     (bootstrapNodes, mountKad, mountServiceDiscovery, ...).
  LOGOSCORE_FLAKE    default: $LOGOSCORE_FLAKE
  LGPM_FLAKE         default: $LGPM_FLAKE
  LIBP2P_FLAKE       default: $LIBP2P_FLAKE
EOF
}

die() { echo "error: $*" >&2; exit 1; }
say() { echo "==> $*"; }

# ---------------------------------------------------------------- arguments --

[ $# -gt 0 ] || { usage; exit 1; }
ACTION=$1; shift

CALL_ARGS=
if [ "$ACTION" = call ]; then
  [ $# -gt 0 ] || die "call needs a method name"
  # Everything after `call` belongs to logoscore, not to us.
  CALL_ARGS=$*
  set --
fi

while [ $# -gt 0 ]; do
  case $1 in
    -c|--config)  CONFIG=$2; shift 2 ;;
    -w|--workdir) WORKDIR=$2; shift 2 ;;
    --libp2p)     WANT_LIBP2P=yes; shift ;;
    --no-libp2p)  WANT_LIBP2P=no; shift ;;
    --rebuild)    REBUILD=1; shift ;;
    --clean)      CLEAN=1; shift ;;
    -h|--help)    usage; exit 0 ;;
    *)            die "unknown option: $1" ;;
  esac
done

MODULES=$WORKDIR/modules
DATA=$WORKDIR/data
DAEMON_LOG=$WORKDIR/daemon.log
LOGOSCORE=$WORKDIR/logos/bin/logoscore

# ------------------------------------------------------------------ helpers --

logoscore() {
  [ -x "$LOGOSCORE" ] || die "logoscore not built — run '$(basename "$0") up' first"
  "$LOGOSCORE" "$@"
}

daemon_running() {
  [ -x "$LOGOSCORE" ] || return 1
  "$LOGOSCORE" status >/dev/null 2>&1
}

# libp2p_module is a declared dependency on this branch, so it has to be present
# in the modules dir for every run, whatever the config says: logos-core resolves
# dependencies at load time and only warns if one is missing, which would then
# fail createNode instead. --no-libp2p exists to demonstrate exactly that.
needs_libp2p() {
  [ "$WANT_LIBP2P" != no ]
}

# nix build <flake> into <out-link>, skipping when the link already resolves.
build_into() {
  flake=$1; link=$2; label=$3
  if [ "$REBUILD" = 0 ] && [ -e "$link" ]; then
    say "$label: reusing $(readlink "$link")"
    return 0
  fi
  say "$label: building $flake"
  nix build "$flake" --out-link "$link" \
    || die "failed to build $label from $flake
  If this is a transient GitHub fetch error, retry, or point at a local
  checkout, e.g.  ${label}_FLAKE=/path/to/checkout"
}

# ----------------------------------------------------------------------- up --

do_up() {
  [ -f "$CONFIG" ] || die "config not found: $CONFIG"
  command -v nix >/dev/null 2>&1 || die "nix is required (see docs/run-node.md)"

  if daemon_running; then
    die "a logoscore daemon is already running — '$(basename "$0") down' first"
  fi

  mkdir -p "$WORKDIR" "$DATA"

  build_into "$LOGOSCORE_FLAKE" "$WORKDIR/logos"        LOGOSCORE
  build_into "$LGPM_FLAKE"      "$WORKDIR/lgpm"         LGPM
  build_into "$REPO#lgx"        "$WORKDIR/delivery-lgx" delivery_module
  if needs_libp2p; then
    build_into "$LIBP2P_FLAKE"  "$WORKDIR/libp2p-lgx"   LIBP2P
  fi

  # Seed from the runtime's own bundle: it ships capability_module, which the
  # daemon needs to hand out inter-module tokens. -L because the bundle is a
  # tree of nix store symlinks and lgpm writes into this directory.
  # Nix store files are read-only and `cp -RL` preserves that, so the tree has
  # to be made writable — both for lgpm to install into it and for the rm on the
  # next run to succeed.
  say "installing modules into $MODULES"
  if [ -d "$MODULES" ]; then
    chmod -R u+w "$MODULES" 2>/dev/null || true
    rm -rf "$MODULES"
  fi
  mkdir -p "$MODULES"
  cp -RL "$WORKDIR/logos/modules/." "$MODULES/"
  chmod -R u+w "$MODULES"

  if needs_libp2p; then
    "$WORKDIR/lgpm/bin/lgpm" --modules-dir "$MODULES" --allow-unsigned \
      install --file "$WORKDIR"/libp2p-lgx/*.lgx >/dev/null
  fi
  "$WORKDIR/lgpm/bin/lgpm" --modules-dir "$MODULES" --allow-unsigned \
    install --file "$WORKDIR"/delivery-lgx/*.lgx >/dev/null

  # -D runs the daemon in the foreground rather than self-detaching, so it has
  # to be backgrounded here; nohup keeps it alive once this script exits.
  say "starting daemon (log: $DAEMON_LOG)"
  nohup "$LOGOSCORE" -D -m "$MODULES" --persistence-path "$DATA" \
    > "$DAEMON_LOG" 2>&1 &

  # Wait for it to answer on its socket before driving it.
  i=0
  while ! daemon_running; do
    i=$((i + 1))
    [ "$i" -lt 30 ] || { tail -20 "$DAEMON_LOG" >&2; die "daemon did not come up"; }
    sleep 1
  done

  # No explicit load-module for libp2p_module: it is a declared dependency, so
  # logos-core pulls it in as part of loading delivery_module.
  say "loading delivery_module (libp2p_module comes with it)"
  logoscore load-module delivery_module

  say "createNode with $CONFIG"
  # A node configured for plugin discovery fails here rather than at start if
  # libp2p_module is unreachable, so surface the daemon log on failure.
  if ! logoscore call delivery_module createNode "@$CONFIG"; then
    tail -20 "$DAEMON_LOG" >&2
    die "createNode failed"
  fi

  say "starting the node"
  logoscore call delivery_module start

  echo
  say "node is up. Try:"
  echo "    $0 status"
  echo "    $0 call getNodeInfo listenAddresses"
  echo "    $0 down"
}

# --------------------------------------------------------------------- down --

do_down() {
  if daemon_running; then
    say "stopping"
    logoscore call delivery_module stop >/dev/null 2>&1 || true
    logoscore stop || true
  else
    say "no daemon running"
  fi
  if [ "$CLEAN" = 1 ]; then
    say "removing $WORKDIR"
    rm -rf "$WORKDIR"
  fi
}

# ------------------------------------------------------------------ dispatch --

case $ACTION in
  up)     do_up ;;
  down)   do_down ;;
  status) logoscore status ;;
  logs)   [ -f "$DAEMON_LOG" ] || die "no log at $DAEMON_LOG"; tail -f "$DAEMON_LOG" ;;
  call)
    daemon_running || die "no daemon running — '$(basename "$0") up' first"
    # Unquoted on purpose: split the saved arg string back into words.
    # shellcheck disable=SC2086
    logoscore call delivery_module $CALL_ARGS
    ;;
  -h|--help) usage ;;
  *)      die "unknown command: $ACTION (try --help)" ;;
esac
