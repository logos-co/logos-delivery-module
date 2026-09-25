#!/bin/sh
# Download logosctl — the Logos node daemon, client and package manager — into ./bin.
#
# Unpacks a pinned release for the host OS/arch. Works on Linux
# (x86_64/aarch64) and macOS (Apple Silicon). To move to a newer build, bump
# LOGOSCTL_TAG below. After it finishes:  export PATH="$PWD/bin:$PATH"
set -eu

LOGOSCTL_TAG=${LOGOSCTL_TAG:-0.3.0}

os=$(uname -s | tr '[:upper:]' '[:lower:]')
if [ "$os" = darwin ]; then os=macos; fi
arch=$(uname -m)
if [ "$arch" = arm64 ]; then arch=aarch64; fi

case "$os" in
  linux|macos) ;;
  *) echo "unsupported OS: $os (Linux/macOS only)" >&2; exit 1 ;;
esac

bin="$PWD/bin"
mkdir -p "$bin"

tool=logosctl
echo "Installing $tool $LOGOSCTL_TAG for $os/$arch into $bin ..."
tmp=$(mktemp -d)
curl -fsSL "https://github.com/logos-co/logos-logoscore-cli/releases/download/$LOGOSCTL_TAG/$tool-$arch-$os.tar.gz" \
  | tar xz -C "$tmp"
if [ "$os" = macos ]; then
  # tarball is <tool>-<arch>-macos/{bin,lib,...}; the binary finds its bundled
  # libs/modules relative to its real path, so wrap it (a symlink would break
  # that resolution) rather than linking.
  rm -rf "$bin/$tool-$arch-macos"
  mv "$tmp/$tool-$arch-macos" "$bin/"
  printf '#!/bin/sh\nexec "%s/%s-%s-macos/bin/%s" "$@"\n' "$bin" "$tool" "$arch" "$tool" > "$bin/$tool"
  chmod +x "$bin/$tool"
else
  # tarball is a single <tool>-<arch>.AppImage
  mv "$tmp/$tool-$arch.AppImage" "$bin/$tool"
  chmod +x "$bin/$tool"
fi
rm -rf "$tmp"

echo
echo "Done. Put it on your PATH:"
echo "  export PATH=\"$bin:\$PATH\""
