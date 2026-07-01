#!/bin/sh
set -eu

repo="xaviersupreme/pak"
version="${PAK_VERSION:-latest}"
install_dir="${PAK_INSTALL_DIR:-$HOME/.local/bin}"

if [ -t 1 ] && [ -z "${NO_COLOR:-}" ]; then
    bold="$(printf '\033[1m')"
    dim="$(printf '\033[2m')"
    red="$(printf '\033[31m')"
    green="$(printf '\033[32m')"
    yellow="$(printf '\033[33m')"
    cyan="$(printf '\033[36m')"
    reset="$(printf '\033[0m')"
else
    bold=""
    dim=""
    red=""
    green=""
    yellow=""
    cyan=""
    reset=""
fi

step() { printf '%s==>%s %s\n' "$cyan$bold" "$reset" "$*"; }
ok() { printf '%sok:%s %s\n' "$green$bold" "$reset" "$*"; }
warn() { printf '%swarn:%s %s\n' "$yellow$bold" "$reset" "$*"; }
die() { printf '%spak:%s %s\n' "$red$bold" "$reset" "$*" >&2; exit 1; }

download_file() {
    url="$1"
    out="$2"

    if command -v curl >/dev/null 2>&1; then
        curl -fL "$url" -o "$out"
    elif command -v wget >/dev/null 2>&1; then
        wget -O "$out" "$url"
    else
        die "curl or wget is required"
    fi
}

fetch_url() {
    url="$1"

    if command -v curl >/dev/null 2>&1; then
        curl -fsSL "$url"
    elif command -v wget >/dev/null 2>&1; then
        wget -qO- "$url"
    else
        die "curl or wget is required"
    fi
}

pak_version_from() {
    exe="$1"

    if [ ! -x "$exe" ]; then
        return 1
    fi
    "$exe" --version 2>/dev/null | sed -n 's/^pak //p' | sed -n '1p'
}

latest_version() {
    fetch_url "https://api.github.com/repos/$repo/releases/latest" |
        sed -n 's/.*"tag_name"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' |
        sed -n '1p'
}

os="$(uname -s)"
arch="$(uname -m)"

case "$os" in
    Linux) platform="linux" ;;
    Darwin) platform="macos" ;;
    *) die "unsupported OS: $os" ;;
esac

case "$arch" in
    x86_64|amd64) cpu="x64" ;;
    arm64|aarch64) cpu="arm64" ;;
    *) die "unsupported CPU: $arch" ;;
esac

if [ "$platform" = "linux" ]; then
    asset="pak-linux-$cpu-musl.tar.gz"
else
    asset="pak-macos-$cpu.tar.gz"
fi

if [ "$version" = "latest" ]; then
    step "checking latest release"
    target_version="$(latest_version)"
    [ -n "$target_version" ] || die "could not resolve latest release"
else
    target_version="$version"
fi

target="$install_dir/pak"
installed_version=""
if [ -x "$target" ]; then
    installed_version="$(pak_version_from "$target" || true)"
    if [ -n "$installed_version" ]; then
        if [ "$installed_version" = "$target_version" ]; then
            ok "pak $installed_version is already installed at $target"
            exit 0
        fi
        warn "pak $installed_version is installed at $target; updating to $target_version"
    else
        warn "pak is installed at $target, but its version is unknown; updating to $target_version"
    fi
elif command -v pak >/dev/null 2>&1; then
    found="$(command -v pak)"
    found_version="$(pak_version_from "$found" || true)"
    if [ -n "$found_version" ]; then
        warn "pak $found_version is installed at $found; installing $target_version to $target"
    else
        warn "pak is installed at $found, but its version is unknown; installing $target_version to $target"
    fi
else
    step "pak is not installed; installing $target_version"
fi

url="https://github.com/$repo/releases/download/$target_version/$asset"
tmp="${TMPDIR:-/tmp}/pak-install-$$"
mkdir -p "$tmp" "$install_dir"
trap 'rm -rf "$tmp"' EXIT INT TERM

step "downloading $asset"
download_file "$url" "$tmp/$asset"

step "installing pak to $install_dir"
tar -xzf "$tmp/$asset" -C "$tmp"
install -m 755 "$tmp/pak" "$target"

installed_version="$(pak_version_from "$target" || true)"
if [ -n "$installed_version" ]; then
    ok "installed pak $installed_version"
else
    ok "installed pak"
fi

case ":$PATH:" in
    *":$install_dir:"*) ;;
    *) warn "make sure $install_dir is in PATH" ;;
esac

if command -v pak >/dev/null 2>&1; then
    active="$(command -v pak)"
    if [ "$active" != "$target" ]; then
        warn "current PATH finds $active before $target"
    fi
else
    printf '%shint:%s open a new terminal after adding %s to PATH\n' "$dim" "$reset" "$install_dir"
fi
