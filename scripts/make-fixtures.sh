#!/usr/bin/env bash
# Synthesizes the NTSC + PAL colour-bars TBC fixtures that test_integration
# reads through CHD_ENCODE_ORC_FIXTURE_DIR.
#
#   scripts/make-fixtures.sh <output-dir> [encode-orc] [assets-dir]
#
# encode-orc defaults to $CHD_ENCODE_ORC and the assets directory to
# $CHD_ENCODE_ORC_ASSETS, falling back to the `assets` beside the binary's
# grandparent, which is where a stock clone-and-build leaves it.
#
# The layout written here is the one resolvePrebuiltFixture() in
# tests/unit/test_integration.cpp expects:
#
#     <output-dir>/<label>/fixture.tbc
#     <output-dir>/<label>/fixture.tbc.db
#
# The project YAML below mirrors renderProjectYaml() in that same file, which
# is what the local-dev path (CHD_ENCODE_ORC) feeds encode-orc. The two have to
# describe the same signal or a fixture built here would not be the one a
# developer reproduces locally.
set -euo pipefail

out_dir="${1:-}"
encode_orc="${2:-${CHD_ENCODE_ORC:-}}"
assets="${3:-${CHD_ENCODE_ORC_ASSETS:-}}"

if [ -z "$out_dir" ]; then
    printf 'usage: %s <output-dir> [encode-orc] [assets-dir]\n' "$0" >&2
    exit 2
fi
if [ -z "$encode_orc" ]; then
    printf 'no encode-orc binary: pass one or set CHD_ENCODE_ORC\n' >&2
    exit 2
fi
if [ ! -x "$encode_orc" ]; then
    printf 'encode-orc is not executable: %s\n' "$encode_orc" >&2
    exit 1
fi
if [ -z "$assets" ]; then
    assets="$(cd "$(dirname "$encode_orc")/../.." && pwd)/assets"
fi
if [ ! -d "$assets" ]; then
    printf 'assets directory does not exist: %s\n' "$assets" >&2
    exit 1
fi

mkdir -p "$out_dir"
out_dir="$(cd "$out_dir" && pwd)"

# label|encode-orc format|asset path relative to the assets root|frames
encoders=(
    'ntsc-bars|ntsc-composite|ntsc-raw/525_5994_75_BARS.raw|3'
    'pal-bars|pal-composite|pal-raw/625_50_75_BARS.raw|3'
)

for spec in "${encoders[@]}"; do
    IFS='|' read -r label format asset frames <<<"$spec"
    sub="$out_dir/$label"
    mkdir -p "$sub"

    # ENCODE_ORC_OUTPUT_ROOT and ENCODE_ORC_ASSETS are expanded by encode-orc
    # itself, so they stay literal in the file and are escaped from the shell.
    cat >"$sub/project.yaml" <<EOF
name: "chd-integration-$label"
output:
  filename: "\${ENCODE_ORC_OUTPUT_ROOT}/fixture"
  format: "$format"
  writer: "tbc"
  metadata_decoder: "encode-orc"
laserdisc:
  mode: "cav"
pipeline:
  preprocessing:
    filters:
      chroma:
        enabled: true
      luma:
        enabled: false
sections:
  - name: "Bars"
    duration: $frames
    source:
      type: "yuv422-image"
      file: "\${ENCODE_ORC_ASSETS}/$asset"
EOF

    ENCODE_ORC_OUTPUT_ROOT="$sub" ENCODE_ORC_ASSETS="$assets" \
        "$encode_orc" "$sub/project.yaml" --log-level warn

    for f in fixture.tbc fixture.tbc.db; do
        if [ ! -s "$sub/$f" ]; then
            printf 'encode-orc produced no %s for %s\n' "$f" "$label" >&2
            exit 1
        fi
    done
    # project.yaml names a path on this machine; it would only mislead a
    # consumer of the artifact.
    rm -f "$sub/project.yaml"
    printf '%s: %s\n' "$label" "$(du -h "$sub/fixture.tbc" | cut -f1)"
done

printf 'fixtures written to %s\n' "$out_dir"
