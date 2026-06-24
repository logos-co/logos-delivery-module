#!/bin/sh
# Download the Logos node CLIs — logoscore, lgpd, lgpm — into ./bin.
#
# Resolves each tool's newest release (they currently ship as pre-releases) and
# unpacks the build for the host OS/arch. Works on Linux (x86_64/aarch64) and
# macOS (Apple Silicon). After it finishes:  export PATH="$PWD/bin:$PATH"
set -eu

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

# newest release tag for a logos-co repo (includes pre-releases)
latest() {
  curl -fsSL "https://api.github.com/repos/logos-co/$1/releases" \
    | grep -m1 '"tag_name":' | cut -d'"' -f4
}

# fetch <repo> <tool>: download <tool>-<arch>-<os>.tar.gz and expose ./bin/<tool>
fetch() {
  repo=$1; tool=$2
  tag=$(latest "$repo")
  [ -n "$tag" ] || { echo "no release found for $repo" >&2; exit 1; }
  echo "  $tool ($tag)"
  tmp=$(mktemp -d)
  curl -fsSL "https://github.com/logos-co/$repo/releases/download/$tag/$tool-$arch-$os.tar.gz" \
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
}

echo "Installing Logos node tools for $os/$arch into $bin ..."
fetch logos-logoscore-cli      logoscore
fetch logos-package-downloader lgpd
fetch logos-package-manager    lgpm
echo
echo "Done. Put them on your PATH:"
echo "  export PATH=\"$bin:\$PATH\""
