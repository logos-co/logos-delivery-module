#!/usr/bin/env bash
#
# Local preview of the versioned docs site:
#
#   ./docs/preview.sh          # build and serve on http://localhost:8000
#
# Needs doxygen on PATH and the docs/requirements.txt packages installed.
#
set -euo pipefail
cd "$(dirname "$0")/.."

VERSION="$(git describe --tags --abbrev=0 2>/dev/null || echo preview)"

echo "==> Running Doxygen…"
doxygen ./docs/Doxyfile

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

echo "==> Serving on http://localhost:8000  (Ctrl-C to stop)"
python3 -m http.server -d docs/site 8000
