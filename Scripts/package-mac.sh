#!/bin/bash
# Build an arm64 editor candidate in a new directory; never overwrite a package.
set -euo pipefail
PLUGIN_ROOT="$(cd "$(dirname "$0")/.." && pwd -P)"
UE_ENGINE_ROOT="${UE_ENGINE_ROOT:-/Users/Shared/Epic Games/UE_5.8}"
if [[ $# -ne 1 ]]; then
    echo "Usage: bash Scripts/package-mac.sh /absolute/new-output-directory" >&2
    exit 2
fi
case "$1" in
    /*) OUTPUT_DIR="$1" ;;
    *) echo "Output directory must be absolute." >&2; exit 2 ;;
esac
if [[ -e "$OUTPUT_DIR" || -L "$OUTPUT_DIR" ]]; then
    echo "Output already exists; choose a new directory: $OUTPUT_DIR" >&2
    exit 2
fi
UE_ENGINE_ROOT="$(cd "$UE_ENGINE_ROOT" && pwd -P)"
OUTPUT_DIR="$(python3 -c 'import pathlib,sys; print(pathlib.Path(sys.argv[1]).resolve())' "$OUTPUT_DIR")"
case "$OUTPUT_DIR/" in
    "$PLUGIN_ROOT/"*|"$UE_ENGINE_ROOT/Engine/"*)
        echo "Output must be outside the source plugin and engine directories." >&2
        exit 2 ;;
esac
# mkdir claims this output without accepting a directory created by another run.
mkdir -p "$(dirname "$OUTPUT_DIR")"
mkdir "$OUTPUT_DIR"
trap 'echo "Package failed; retained output: $OUTPUT_DIR" >&2' ERR
"$UE_ENGINE_ROOT/Engine/Build/BatchFiles/RunUAT.sh" BuildPlugin \
    "-Plugin=$PLUGIN_ROOT/GitSourceControl.uplugin" "-Package=$OUTPUT_DIR/Plugin" \
    -NoTargetPlatforms -Architecture_Mac=arm64 > "$OUTPUT_DIR/BuildPlugin.log" 2>&1
python3 "$PLUGIN_ROOT/Scripts/check_mac_package.py" "$OUTPUT_DIR/Plugin" \
    --engine "$UE_ENGINE_ROOT" --source "$PLUGIN_ROOT" --report "$OUTPUT_DIR/PackageAudit.json"
