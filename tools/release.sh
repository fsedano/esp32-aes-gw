#!/usr/bin/env bash

# Build, encrypt, verify, and publish an ESP32 application OTA image.
# Run from anywhere inside/outside the repository:
#
#   tools/release.sh                     # clean build; infer tag from firmware
#   tools/release.sh --allow-dirty       # publish a dirty bench build
#   tools/release.sh --allow-dirty v0.2.3-test.1
#   tools/release.sh --allow-dirty --dry-run

set -euo pipefail

usage() {
    cat <<'EOF'
Usage: tools/release.sh [--allow-dirty] [--dry-run] [RELEASE_TAG]

Builds the application, packs the encrypted OTA image, verifies it, and
creates a GitHub release in fsedano/sim-lc-esp32-aes-gw.

  --allow-dirty  Permit staged/unstaged tracked changes. The release is
                 marked as a prerelease.
  --dry-run      Build and pack, but do not create the GitHub release.
  RELEASE_TAG    Optional GitHub release tag and embedded firmware version.
                 Long Git-derived dirty versions get a compact default.

Environment overrides:
  FW_RELEASE_REPO    GitHub repository (default: fsedano/sim-lc-esp32-aes-gw)
  FW_RELEASE_TARGET  GitHub tag target (default: main)
  FW_RELEASE_BUILD_DIR  Isolated IDF build directory (default: build-release)
  IDF_EXPORT         Path to ESP-IDF export.sh
EOF
}

die() {
    echo "release: $*" >&2
    exit 1
}

allow_dirty=0
dry_run=0
release_tag=""

while (($#)); do
    case "$1" in
        --allow-dirty)
            allow_dirty=1
            ;;
        --dry-run)
            dry_run=1
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        -*)
            die "unknown option: $1 (use --help)"
            ;;
        *)
            [[ -z "$release_tag" ]] || die "only one RELEASE_TAG is allowed"
            release_tag="$1"
            ;;
    esac
    shift
done

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "$script_dir/.." && pwd)"
cd "$repo_root"

git rev-parse --show-toplevel >/dev/null 2>&1 \
    || die "not inside a Git repository"

dirty=0
if ! git diff-index --quiet HEAD --; then
    dirty=1
fi
if ((dirty && !allow_dirty)); then
    die "tracked files are dirty; commit them or rerun with --allow-dirty"
fi

# Derive the normal Git version first. A chosen release tag is then passed
# back to CMake as the version for both GET_FW_INFO and the ESP-IDF descriptor.
git_version="$(
    cmake -P cmake/gen_version.cmake 2>&1 \
        | sed -n 's/^-- Version: \([^ ]*\).*/\1/p' \
        | tail -n 1
)"
[[ -n "$git_version" ]] || die "could not determine firmware version"

if [[ -z "$release_tag" ]]; then
    release_tag="$git_version"
    # GET_FW_INFO has a 24-byte version field. Compact a long development
    # version while retaining its next-patch base and source commit.
    if ((${#release_tag} > 24)); then
        release_tag="$(
            printf '%s\n' "$git_version" \
                | sed -E 's/^(v[0-9]+\.[0-9]+\.[0-9]+)-dev\.[0-9]+\.g([0-9a-fA-F]{8}).*/\1-d.g\2/'
        )"
    fi
fi
git check-ref-format "refs/tags/$release_tag" >/dev/null 2>&1 \
    || die "invalid release tag: $release_tag"
[[ "$release_tag" =~ ^v[0-9A-Za-z][0-9A-Za-z._-]*$ ]] \
    || die "release tag must begin with v and contain only letters, digits, ., _, or -"
((${#release_tag} <= 24)) \
    || die "release tag exceeds GET_FW_INFO's 24-byte version field"

firmware_version="$release_tag"

release_repo="${FW_RELEASE_REPO:-fsedano/sim-lc-esp32-aes-gw}"
release_target="${FW_RELEASE_TARGET:-main}"
release_build_dir="${FW_RELEASE_BUILD_DIR:-build-release}"
app_image="$release_build_dir/esp32-aes-gw.bin"
release_image="esp32-fw-Release-${release_tag}.bin"
commit="$(git rev-parse --short=8 HEAD)"

echo "Firmware version : $firmware_version"
echo "Git version basis : $git_version"
echo "Release tag      : $release_tag"
echo "Git commit       : $commit"
echo "Release asset    : $release_image"
if ((dirty)); then
    echo "WARNING: publishing tracked working-tree changes (--allow-dirty)"
fi
if (( ! dry_run )); then
    command -v gh >/dev/null 2>&1 || die "GitHub CLI (gh) is not installed"
    gh auth status -h github.com >/dev/null 2>&1 \
        || die "GitHub CLI is not authenticated; run: gh auth login"
    if gh release view "$release_tag" -R "$release_repo" >/dev/null 2>&1; then
        die "release $release_tag already exists in $release_repo"
    fi
fi

# Source ESP-IDF when the caller has not already activated it. On machines
# whose system Python is newer than ESP-IDF supports, prefer the installed
# IDF 5.5 Python environment before sourcing export.sh.
if ! command -v idf.py >/dev/null 2>&1; then
    idf_tools_root="${IDF_TOOLS_PATH:-${HOME}/.espressif}"
    for idf_env_bin in "$idf_tools_root"/python_env/idf5.5_py*_env/bin; do
        if [[ -x "$idf_env_bin/python" ]]; then
            PATH="$idf_env_bin:$PATH"
            break
        fi
    done

    idf_export="${IDF_EXPORT:-${IDF_PATH:-${HOME}/esp/esp-idf}/export.sh}"
    [[ -f "$idf_export" ]] \
        || die "ESP-IDF export script not found: $idf_export"
    # shellcheck disable=SC1090
    source "$idf_export"
fi
command -v idf.py >/dev/null 2>&1 || die "idf.py is unavailable"

python -c 'import cryptography' >/dev/null 2>&1 \
    || die "Python package 'cryptography' is missing; run: pip install cryptography"

idf.py -B "$release_build_dir" \
    -DFW_VERSION_OVERRIDE="$firmware_version" reconfigure build
[[ -f "$app_image" ]] || die "application build did not produce $app_image"

# Confirm the version used by GET_FW_INFO is actually present in the app.
strings "$app_image" | grep -Fx "$firmware_version" >/dev/null \
    || die "embedded firmware version $firmware_version not found in $app_image"

python tools/fwpack.py "$app_image" "$release_image"

python - "$release_image" <<'PY'
import struct
import sys

path = sys.argv[1]
with open(path, "rb") as stream:
    image = stream.read()

assert len(image) >= 24, "container is shorter than its header"
size, fletcher = struct.unpack("<II", image[:8])
assert size == len(image) - 24, "header size does not match ciphertext"
assert size % 16 == 0, "ciphertext is not AES-block aligned"
assert len(image) <= 1 << 20, "container exceeds aes-gw2's 1 MiB limit"
print(
    f"Verified          : {len(image)} bytes, ciphertext {size}, "
    f"fletcher32 0x{fletcher:08x}"
)
PY

if ((dry_run)); then
    echo "Dry run complete; GitHub release was not created."
    exit 0
fi

notes="AES-ESP-DO32-HID firmware $release_tag"
if ((dirty)); then
    notes+=$'\n\nBench/test image built with tracked working-tree changes.'
    notes+=$'\nEmbedded wire version: `'
    notes+="$firmware_version"
    notes+=$'`\nSource commit: `'
    notes+="$commit"
    notes+=$'`'
fi

gh_args=(
    release create "$release_tag"
    -R "$release_repo"
    --target "$release_target"
    --title "$release_tag"
    --notes "$notes"
)
if ((dirty)) || [[ "$firmware_version" == *-* ]]; then
    gh_args+=(--prerelease)
fi
gh_args+=("$release_image")

gh "${gh_args[@]}"
echo "Published         : https://github.com/$release_repo/releases/tag/$release_tag"
