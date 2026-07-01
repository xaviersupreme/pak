#!/bin/sh
set -eu

repo="xaviersupreme/pak"
version="${PAK_VERSION:-latest}"
install_dir="${PAK_INSTALL_DIR:-$HOME/.local/bin}"

os="$(uname -s)"
arch="$(uname -m)"

case "$os" in
    Linux) platform="linux" ;;
    Darwin) platform="macos" ;;
    *) echo "pak: unsupported OS: $os" >&2; exit 1 ;;
esac

case "$arch" in
    x86_64|amd64) cpu="x64" ;;
    arm64|aarch64) cpu="arm64" ;;
    *) echo "pak: unsupported CPU: $arch" >&2; exit 1 ;;
esac

if [ "$platform" = "linux" ]; then
    asset="pak-linux-$cpu-musl.tar.gz"
else
    asset="pak-macos-$cpu.tar.gz"
fi

if [ "$version" = "latest" ]; then
    url="https://github.com/$repo/releases/latest/download/$asset"
else
    url="https://github.com/$repo/releases/download/$version/$asset"
fi

tmp="${TMPDIR:-/tmp}/pak-install-$$"
mkdir -p "$tmp" "$install_dir"
trap 'rm -rf "$tmp"' EXIT INT TERM

echo "==> downloading $asset"
if command -v curl >/dev/null 2>&1; then
    curl -fL "$url" -o "$tmp/$asset"
elif command -v wget >/dev/null 2>&1; then
    wget -O "$tmp/$asset" "$url"
else
    echo "pak: curl or wget is required" >&2
    exit 1
fi

echo "==> installing pak to $install_dir"
tar -xzf "$tmp/$asset" -C "$tmp"
install -m 755 "$tmp/pak" "$install_dir/pak"

echo "==> installed $("$install_dir/pak" --version 2>/dev/null || echo pak)"
echo "hint: make sure $install_dir is in PATH"
