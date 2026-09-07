#!/usr/bin/env bash
#
# Local preview of the docs:
#
#   ./docs/preview.sh            # build the versioned site once and serve it
#   ./docs/preview.sh --watch    # rebuild and reload the browser as sources change
#   PORT=8123 ./docs/preview.sh  # serve somewhere other than 8000
#
# Both serve on http://localhost:$PORT and need doxygen on PATH. --watch also
# needs docs/requirements-dev.txt; the one-shot build only needs
# docs/requirements.txt.
#
# --watch serves the bare HTML rather than the gh-pages tree, so the version
# dropdown stays empty. Use the one-shot build to check that.
#
set -euo pipefail
cd "$(dirname "$0")/.."

PORT="${PORT:-8000}"

WATCH=""
case "${1:-}" in
  --watch) WATCH=1 ;;
  "")      ;;
  *)       echo "usage: ${0} [--watch]" >&2; exit 1 ;;
esac

echo "==> Running Doxygen…"
doxygen ./docs/Doxyfile

if [ -n "$WATCH" ]; then
  echo "==> Watching for changes, serving on http://localhost:$PORT  (Ctrl-C to stop)"
  # Doxygen rewrites docs/xml on every pre-build, so watching it would loop.
  exec sphinx-autobuild docs docs/_build/html \
    --host 0.0.0.0 \
    --port "$PORT" \
    --watch src \
    --pre-build "doxygen ./docs/Doxyfile" \
    --ignore '*/xml/*' \
    --ignore '*/_build/*' \
    --ignore '*/site/*'
fi

VERSION="$(git describe --tags --abbrev=0 2>/dev/null || echo preview)"

echo "==> Building docs (clean)…"
make -C docs clean
# Root-relative switcher URL so the dropdown fetch is same-origin locally,
# whatever host you browse from (localhost / 127.0.0.1 / 0.0.0.0).
SWITCHER_JSON_URL=/switcher.json make -C docs html

echo "==> Assembling gh-pages-like tree in ./docs/site…"
rm -rf docs/site && mkdir docs/site
cp -r docs/_build/html "docs/site/$VERSION"
cp -r docs/_build/html docs/site/latest
cp docs/_root/index.html docs/site/index.html
cp docs/_root/switcher.json docs/site/switcher.json

echo "==> Serving on http://localhost:$PORT  (Ctrl-C to stop)"
python3 -m http.server -d docs/site "$PORT"
