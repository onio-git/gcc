#!/usr/bin/env bash
# Fetch a pinned embsim release binary and verify it.
#
# embsim lives in a private repository, so its release assets are not
# reachable from the browser download URL -- that returns 404 without
# credentials, which looks like a missing file rather than a missing token.
# The API asset endpoint is the one that works, and it needs a token with read
# access to mikro-design/embsim: either `gh auth login`, or GH_TOKEN in the
# environment.
#
# Usage: fetch-embsim.sh [version] [destination]
#        EMBSIM=$(contrib/onio-zero/fetch-embsim.sh) ./run-coremark-embsim.py ...
set -euo pipefail

REPO=mikro-design/embsim
VERSION="${1:-${EMBSIM_VERSION:-v0.2.0}}"
DEST="${2:-${EMBSIM_CACHE:-$HOME/.cache/embsim}}"

case "$(uname -s)-$(uname -m)" in
  Linux-x86_64)   ASSET=embsim-linux-x86_64.tar.gz ;;
  Darwin-arm64)   ASSET=embsim-macos-aarch64.tar.gz ;;
  Darwin-x86_64)  ASSET=embsim-macos-x86_64.tar.gz ;;
  *) echo "no published binary for $(uname -s)-$(uname -m); build from source" >&2; exit 1 ;;
esac

install_dir="$DEST/$VERSION"
binary="$install_dir/${ASSET%%.tar.gz}/embsim"
if [ -x "$binary" ]; then
  echo "$binary"
  exit 0
fi

command -v gh >/dev/null || { echo "gh is required to read a private release" >&2; exit 1; }

mkdir -p "$install_dir"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

# Resolve the asset ids for the tag, then pull the bytes. `gh release download`
# also works; this spells out the endpoint because the browser URL does not.
ids=$(gh api "repos/$REPO/releases/tags/$VERSION" \
      --jq ".assets[] | select(.name == \"$ASSET\" or .name == \"$ASSET.sha256\") | \"\(.name) \(.id)\"")
[ -n "$ids" ] || { echo "no asset $ASSET on $VERSION of $REPO" >&2; exit 1; }

while read -r name id; do
  [ -n "$name" ] || continue
  gh api -H "Accept: application/octet-stream" "repos/$REPO/releases/assets/$id" > "$tmp/$name"
done <<< "$ids"

# Verify before unpacking, not after. A tarball that fails its own checksum is
# not something to run and then check.
( cd "$tmp" && sha256sum -c "$ASSET.sha256" >/dev/null ) \
  || { echo "$ASSET failed its published checksum" >&2; exit 1; }

tar xzf "$tmp/$ASSET" -C "$install_dir"
[ -x "$binary" ] || { echo "no embsim in $ASSET" >&2; exit 1; }

# Say which one, so a log records the version that produced the numbers.
"$binary" --version >&2
echo "$binary"
