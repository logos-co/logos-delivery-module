#!/usr/bin/env bash
# Builds every Linux artefact of the sim image inside nixos/nix (persistent
# store in the docker volume logos-sim-nix, so reruns are incremental) and
# copies them to ./out, then builds the runtime image logos-sim:local.
#
#   logoscore        cli-bundle-dir of logos-logoscore-cli (portable bundle)
#   delivery_module  install-portable of THIS working tree (dirty state included)
#   libp2p_module    install-portable at the rev the module's flake.lock pins
#   openmetrics      install-portable at the rev flake.lock pins (serves the
#                    /metrics endpoint each member is scraped on)
#   logosdeliverynode  the seed, from the logos-delivery rev flake.nix pins
#
# Usage: build.sh [--no-image]
set -euo pipefail
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo=$(cd "$here/../.." && pwd)

LOGOSCORE_CLI_REV=${LOGOSCORE_CLI_REV:-665ac28bc77f45e3c610f40fef975a12609de531}
BUILDER_IMAGE=${BUILDER_IMAGE:-nixos/nix:2.24.9}
NIX_VOLUME=${NIX_VOLUME:-logos-sim-nix}

delivery_url=$(grep -o 'logos-delivery.url = "[^"]*"' "$repo/flake.nix" | sed 's/.*= "\(.*\)"/\1/')
locked_rev() { python3 -c 'import json,sys;print(json.load(open(sys.argv[1]))["nodes"][sys.argv[2]]["locked"]["rev"])' "$repo/flake.lock" "$1"; }
libp2p_rev=$(locked_rev libp2p_module)
openmetrics_rev=$(locked_rev openmetrics-module)
[ -n "$delivery_url" ] && [ -n "$libp2p_rev" ] && [ -n "$openmetrics_rev" ]

mkdir -p "$here/out"
# The module source as the daemon should see it: tracked + untracked-but-not-
# ignored files of the working tree (no build/, .local/, result links).
( cd "$repo" && git ls-files -co --exclude-standard -z | tar --null -T - -cf "$here/out/src.tar" )

echo "==> nix builds in $BUILDER_IMAGE (store volume $NIX_VOLUME)"
docker run --rm \
  -v "$NIX_VOLUME:/nix" \
  -v "$here/out:/out" \
  -e DELIVERY_URL="$delivery_url" \
  -e LIBP2P_REV="$libp2p_rev" \
  -e OPENMETRICS_REV="$openmetrics_rev" \
  -e LOGOSCORE_CLI_REV="$LOGOSCORE_CLI_REV" \
  "$BUILDER_IMAGE" sh -euc '
    mkdir -p /etc/nix
    {
      echo "experimental-features = nix-command flakes"
      echo "sandbox = false"
      echo "filter-syscalls = false"
      echo "extra-substituters = https://cache.nix.logos.co/public https://cache.nix.logos.co/ci"
      echo "extra-trusted-public-keys = public:l4HrXgL4nw246+LBh2SOJyhz64BoGegOYLheT/iIAPU= ci:aVJqjS4NWX5WfqHO0AEhIScjGu/JyK5FoydPrnEbMvc="
      echo "fallback = true"
    } > /etc/nix/nix.conf
    rm -rf /tmp/src && mkdir -p /tmp/src && tar -xf /out/src.tar -C /tmp/src
    echo "--- logoscore cli-bundle-dir @ $LOGOSCORE_CLI_REV"
    nix build -L "github:logos-co/logos-logoscore-cli/$LOGOSCORE_CLI_REV#cli-bundle-dir" -o /tmp/r-core
    echo "--- libp2p_module install-portable @ $LIBP2P_REV"
    nix build -L "git+https://github.com/logos-co/logos-libp2p-module?rev=$LIBP2P_REV#install-portable" -o /tmp/r-p2p
    echo "--- openmetrics install-portable @ $OPENMETRICS_REV"
    nix build -L "github:logos-co/openmetrics-module/$OPENMETRICS_REV#install-portable" -o /tmp/r-om
    echo "--- delivery_module install-portable (working tree)"
    nix build -L "path:/tmp/src#install-portable" -o /tmp/r-dm
    echo "--- logosdeliverynode seed @ $DELIVERY_URL"
    nix build -L "$DELIVERY_URL#logosdeliverynode" -o /tmp/r-seed
    echo "--- collecting into /out"
    chmod -R u+w /out/logoscore /out/modules /out/seed-store 2>/dev/null || true
    rm -rf /out/logoscore /out/modules /out/seed-store
    cp -rL /tmp/r-core /out/logoscore
    mkdir -p /out/modules
    cp -rL /tmp/r-core/modules/. /out/modules/
    cp -rL /tmp/r-p2p/modules/. /out/modules/
    cp -rL /tmp/r-om/modules/. /out/modules/
    cp -rL /tmp/r-dm/modules/. /out/modules/
    mkdir -p /out/seed-store
    for p in $(nix path-info -r /tmp/r-seed); do cp -a "$p" /out/seed-store/; done
    readlink -f /tmp/r-seed > /out/seed-path
    chmod -R u+w /out/logoscore /out/modules /out/seed-store
    echo "modules:"; ls /out/modules
    echo "seed: $(cat /out/seed-path)  closure: $(du -sh /out/seed-store | cut -f1)"
  '

if [ "${1:-}" != "--no-image" ]; then
  echo "==> docker build logos-sim:local"
  docker build -t logos-sim:local "$here"
fi
