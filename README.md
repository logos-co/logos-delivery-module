# logos-delivery-module

Wrap LogosMessaging API (liblogosdelivery) and make it available as a Logos Core module.

This module provides high-level message delivery capabilities through the liblogosdelivery interface from [logos-delivery](https://github.com/logos-messaging/logos-delivery), packaged as a Logos module plugin compatible with logos-core.

## Documentation

The module's documentation is published at
**<https://logos-co.github.io/logos-delivery-module/>** — API reference,
configuration, events, and the guides for running and querying a node.

The API reference is generated from the doc comments in
[`src/delivery_module_plugin.h`](src/delivery_module_plugin.h), so it is the
one place the API surface is described. Update the doc comments and the site
follows.

### Building the documentation

The site is Doxygen (API extraction) → Breathe → Sphinx (rendering), with the
Markdown guides in `docs/` pulled in via myst-parser.

`doxygen` is not in the dev shell, so install it once:

```bash
sudo apt-get install -y doxygen     # macOS: brew install doxygen
```

Then:

```bash
python3 -m venv .venv && source .venv/bin/activate
pip install -r docs/requirements.txt
./docs/preview.sh                    # build and serve on http://localhost:8000
```

While writing, `--watch` rebuilds and reloads the browser on every save,
including changes to the doc comments in `src/`:

```bash
pip install -r docs/requirements-dev.txt
./docs/preview.sh --watch
```

Set `PORT` to serve somewhere other than 8000. `--watch` serves the bare HTML,
so the version dropdown stays empty — use the one-shot build to check that.

To build without serving:

```bash
doxygen ./docs/Doxyfile              # writes docs/xml
make -C docs html                    # writes docs/_build/html
```

`make html` runs with `-W`, so a broken link or reference fails the build.
`--watch` does not, so a half-written link doesn't kill the server.

Publishing is automatic: `.github/workflows/docs.yml` deploys to the
`gh-pages` branch when a release is published, under `latest/` and the release
tag. Pushing to a branch builds the site and uploads it as a `docs-preview`
artifact instead, so a docs change can be previewed before it ships. Adding a
new release to the version dropdown means editing
[`docs/_root/switcher.json`](docs/_root/switcher.json).

## How to Build

### Using Nix (Recommended)

#### Build Complete Module (Library + Headers)

```bash
# Build everything (default)
nix build
```

The result will include:
- `/lib/delivery_module_plugin.dylib` (or `.so` on Linux) - The Delivery module plugin
- `/lib/liblogosdelivery.dylib` (or `.so` on linux) - The logos-delivery library
- `/lib/librln.dylib` (or `.so` in linux) - Zerokit's RLN library
- `/lib/libpq.dylib` (or `.so` on Linux) - PostgreSQL runtime library
- `/lib/libpq.5.dylib` (or `.so.5` on Linux)

#### Build Individual Components

```bash
# Build only the library (plugin + liblogosdelivery reference)
nix build '.#lib'

# Build only the generated headers
nix build '.#include'

# build module in local and in protable logos_core format
nix build .#lgx / .#lgx-portable
```

#### Development Shell

```bash
# Enter development shell with all dependencies
nix develop
```

**Note:** In zsh, you need to quote the target (e.g., `'.#default'`) to prevent glob expansion.

If you don't have flakes enabled globally, add experimental flags:

```bash
nix build --extra-experimental-features 'nix-command flakes'
```

The compiled artifacts can be found at `result/`

## Output Structure

When built with Nix, the module produces:

```
result/
└── lib/
    ├── delivery_module_plugin.dylib  # or .so on Linux — Logos module plugin
    ├── liblogosdelivery.dylib
    ├── librln.dylib
    ├── libpq.dylib                   # or .so on Linux — PostgreSQL runtime
    └── libpq.5.dylib                 # or .so.5 on Linux
```

### Requirements

#### Build Tools
- CMake (3.14 or later)
- Ninja build system
- pkg-config

#### Dependencies
- Qt6 (qtbase)
- Qt6 Remote Objects (qtremoteobjects)
- logos-liblogos (provided via Nix)
- logos-cpp-sdk (provided via Nix)
- logos-delivery / liblogosdelivery — target (provided via Nix)
- PostgreSQL (libpq) — runtime dependency bundled by the Nix build

All dependencies are automatically handled by the Nix flake configuration.

## Development

### Local Development

```bash
# Enter development shell (exports LOGOS_MODULE_BUILDER_ROOT and all other deps)
nix develop

# Configure — env vars are exported automatically by the dev shell
cmake -B build -S . -GNinja

# Build
ninja -C build
```
