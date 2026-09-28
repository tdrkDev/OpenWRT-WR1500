#!/bin/bash

usage() {
    echo "Usage: $0 <openwrt build dir> <packages repo dir> [version]"
    echo
    echo "  openwrt build dir  local or ssh path, e.g. tdrk@teslogic:openwrt (must contain stock/releases/)"
    echo "  packages repo dir  local or ssh path served as https://packages.tdrk.dev/"
    echo "  version            release name, e.g. wr1500-2026.09.27-08.13-snapshot (default: newest)"
    echo
    echo "Env: PACKAGES_RSH  ssh command for the packages host, e.g. 'ssh -p 2222' (default: RSYNC_RSH or ssh)"
    echo "     RSYNC_RSH     ssh command for every rsync, including the build host"
    echo "     TARGET        commit or branch to tag, already pushed to GitHub (default: main)"
    echo "     EDITOR        editor for the release notes (default: nano)"
    exit 1
}

WRT_SOURCE="$1"
[ -z "$WRT_SOURCE" ] && usage
WRT_PACKAGES_REPO="$2"
[ -z "$WRT_PACKAGES_REPO" ] && usage
VERSION="$3"

# Local paths are relative to the caller's directory: resolve them before the cd below
abspath() {
    case "$1" in
    *:*) echo "$1" ;;
    *) (cd "$1" && pwd) ;;
    esac
}
WRT_SOURCE="$(abspath "$WRT_SOURCE")" || exit 1
WRT_PACKAGES_REPO="$(abspath "$WRT_PACKAGES_REPO")" || exit 1

RELEASES_DIR="$WRT_SOURCE/stock/releases"

set -e
cd "$(dirname "$(realpath "$0")")"

for cmd in rsync jq zip gh shasum; do
    command -v "$cmd" >/dev/null || { echo "Error: $cmd not found"; exit 1; }
done
gh auth status >/dev/null 2>&1 || { echo "Error: gh is not logged in (gh auth login)"; exit 1; }

if [ -z "$VERSION" ]; then
    VERSION="$(rsync --list-only "$RELEASES_DIR/github/" | awk '$1 ~ /^d/ && $NF != "." { print $NF }' | sort | tail -n 1)"
    [ -z "$VERSION" ] && { echo "Error: No releases found in $RELEASES_DIR/github/"; exit 1; }
fi
# Folders, tags and the package URL use the lowercase name; the build prints it in uppercase
VERSION="$(tr '[:upper:]' '[:lower:]' <<< "$VERSION")"

if gh release view "$VERSION" >/dev/null 2>&1; then
    echo "Error: GitHub release $VERSION already exists"
    exit 1
fi

if ! grep -qi 'wr1500' <<< "$VERSION"; then
    echo "Error: Version does not contain 'wr1500'"
    exit 1
fi

echo "Syncing package repo..."
mkdir -p tmp/repo tmp/github
rsync -az --progress --delete "$RELEASES_DIR/repo/$VERSION/" tmp/repo/

echo "Syncing GitHub release files..."
rsync -az --progress --delete "$RELEASES_DIR/github/$VERSION/" tmp/github/

PROFILES_VERSION="$(jq .version_number --raw-output "tmp/github/profiles.json")"
if [ "$(tr '[:upper:]' '[:lower:]' <<< "$PROFILES_VERSION")" != "$(tr '[:upper:]' '[:lower:]' <<< "$VERSION")" ]; then
    echo "Error: profiles.json version '$PROFILES_VERSION' does not match '$VERSION'"
    exit 1
fi
echo "> Working with version $VERSION"

echo "Verifying checksums..."
(cd tmp/github && shasum -a 256 --ignore-missing -c sha256sums)

echo "Creating easy install ZIP..."
EASY="install_wr1500_$VERSION"
rm -rf tmp/easy tmp/release
mkdir -p "tmp/easy/$EASY" tmp/release
cp tmp/github/*-squashfs-recovery.bin "tmp/easy/$EASY/recovery.bin"
cp tftp.sh tftp.bat recovery_install.py README.md README.ru.md cudy.png cudy-ru.png "tmp/easy/$EASY/"
(cd tmp/easy && zip -r -X "../release/$EASY.zip" "$EASY")

cat > tmp/NOTES.md <<EOF
## OpenWrt $PROFILES_VERSION

### Changes

- 

### Install / update

- New install: \`install_wr1500_$VERSION.zip\`, see the README
- Update from OpenWrt: \`*-squashfs-sysupgrade.bin\` in LuCI (settings can be kept)
EOF
cp tmp/NOTES.md tmp/NOTES.orig
${EDITOR:-nano} tmp/NOTES.md
if [ ! -s tmp/NOTES.md ] || cmp -s tmp/NOTES.md tmp/NOTES.orig; then
    echo "Error: Release notes are empty or unchanged"
    exit 1
fi

echo "Uploading repo to https://packages.tdrk.dev/$VERSION/ ..."
rsync -az --progress --delete -e "${PACKAGES_RSH:-${RSYNC_RSH:-ssh}}" tmp/repo/ "$WRT_PACKAGES_REPO/$VERSION/"

echo "Uploading release..."
gh release create \
    "$VERSION" \
    tmp/github/* \
    "tmp/release/install_wr1500_$VERSION.zip" \
    --target "${TARGET:-main}" \
    --title "OpenWrt $PROFILES_VERSION" \
    --notes-file "tmp/NOTES.md"
