#!/usr/bin/env bash
# Gates the version copies that cannot read VERSION.txt for themselves.
#
# meson.build and nix/libchromadec.nix both read VERSION.txt directly, so they
# cannot drift. Cargo has no way to source a version from a file, and a release
# tag is chosen by whoever pushes it, so those are checked here instead.
#
#   scripts/check-version.sh          # check the in-tree copies agree
#   scripts/check-version.sh v0.2.0   # also require a release tag to match
#
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
status=0

fail() {
    printf 'version drift: %s\n' "$1" >&2
    status=1
}

version="$(tr -d '[:space:]' < "$root/VERSION.txt")"
if [ -z "$version" ]; then
    printf 'VERSION.txt is empty\n' >&2
    exit 1
fi
# Not full semver: just enough to catch a stray "v" prefix or a truncated edit,
# either of which would produce a nonsense soversion.
if ! printf '%s' "$version" | grep -Eq '^[0-9]+\.[0-9]+\.[0-9]+([-+][0-9A-Za-z.-]+)?$'; then
    printf 'VERSION.txt is not a MAJOR.MINOR.PATCH version: %s\n' "$version" >&2
    exit 1
fi
printf 'VERSION.txt: %s\n' "$version"

# meson.build must keep deriving from VERSION.txt rather than reintroducing a
# literal, which would drift silently since nothing downstream would notice.
if ! grep -q "version: files('VERSION.txt')" "$root/meson.build"; then
    fail "meson.build no longer reads version from VERSION.txt (expected \"version: files('VERSION.txt')\")"
fi

# [workspace.package] version, inherited by both crates via version.workspace.
cargo_ws="$(awk '
    /^\[/ { in_section = ($0 == "[workspace.package]") }
    in_section && /^version[[:space:]]*=/ {
        gsub(/^version[[:space:]]*=[[:space:]]*"|"[[:space:]]*$/, "")
        print
        exit
    }
' "$root/rust/Cargo.toml")"
if [ "$cargo_ws" != "$version" ]; then
    fail "rust/Cargo.toml [workspace.package] version is $cargo_ws"
fi

# The intra-workspace path dependency carries its own version requirement, which
# has to move with the crate it points at or `cargo publish` rejects it.
cargo_sys="$(sed -n 's/.*chromadec-sys[[:space:]]*=[[:space:]]*{.*version[[:space:]]*=[[:space:]]*"\([^"]*\)".*/\1/p' \
    "$root/rust/chromadec/Cargo.toml")"
if [ "$cargo_sys" != "$version" ]; then
    fail "rust/chromadec/Cargo.toml chromadec-sys dependency requires $cargo_sys"
fi

if [ "$#" -ge 1 ]; then
    tag_version="${1#v}"
    printf 'release tag: %s\n' "$1"
    if [ "$tag_version" != "$version" ]; then
        fail "release tag $1 does not match VERSION.txt $version"
    fi
fi

if [ "$status" -eq 0 ]; then
    printf 'all version copies agree on %s\n' "$version"
fi
exit "$status"
