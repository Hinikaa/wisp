#!/bin/sh
# Install prebuilt static-Lua wisp binary from GitHub Releases.
# Usage:
#   curl -fsSL https://raw.githubusercontent.com/Hinikaa/wisp/master/install.sh | sh
#   sh install.sh [--prefix /usr/local] [--version v3.2.0]

set -eu

REPO="Hinikaa/wisp"
VERSION=""
if [ "$(id -u)" -eq 0 ]; then
    PREFIX="/usr/local"
else
    PREFIX="${HOME}/.local"
fi

while [ $# -gt 0 ]; do
    case "$1" in
        --prefix)
            PREFIX="$2"
            shift 2
            ;;
        --version)
            VERSION="$2"
            shift 2
            ;;
        -h|--help)
            printf 'usage: install.sh [--prefix DIR] [--version TAG]\n'
            exit 0
            ;;
        *)
            printf 'install.sh: unknown option: %s\n' "$1" >&2
            exit 1
            ;;
    esac
done

OS=$(uname -s)
if [ "$OS" != "Linux" ]; then
    printf 'install.sh: unsupported OS: %s (Linux only)\n' "$OS" >&2
    exit 1
fi

case "$(uname -m)" in
    x86_64|amd64)  ARCH="x86_64" ;;
    aarch64|arm64) ARCH="aarch64" ;;
    *)
        printf 'install.sh: unsupported architecture: %s\n' "$(uname -m)" >&2
        exit 1
        ;;
esac

if [ -z "$VERSION" ]; then
    VERSION=$(curl -fsSL "https://api.github.com/repos/${REPO}/releases/latest" \
        | sed -n 's/.*"tag_name":[[:space:]]*"\([^"]*\)".*/\1/p' | head -n 1)
    if [ -z "$VERSION" ]; then
        printf 'install.sh: could not determine latest release tag\n' >&2
        exit 1
    fi
fi

URL="https://github.com/${REPO}/releases/download/${VERSION}/wisp-linux-${ARCH}.tar.gz"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

printf 'Downloading wisp %s (%s)...\n' "$VERSION" "$ARCH"
curl -fsSL "$URL" -o "${TMP}/wisp.tar.gz"
tar -xzf "${TMP}/wisp.tar.gz" -C "$TMP"

mkdir -p "${PREFIX}/bin"
install -m 755 "${TMP}/wisp" "${PREFIX}/bin/wisp"

printf 'Installed %s/bin/wisp\n' "$PREFIX"
case ":${PATH}:" in
    *":${PREFIX}/bin:"*) ;;
    *) printf 'Note: add %s/bin to your PATH to run wisp directly.\n' "$PREFIX" ;;
esac
