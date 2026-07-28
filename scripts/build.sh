#!/usr/bin/env bash
# Build the firmware inside WSL (esp-matter requires Linux).
#
#   ./scripts/build.sh [target]     target: esp32c3 (default) | esp32c6
#
# Environment used (verified 2026-07-28):
#   ESP-IDF     : ~/esp/v5.4.2/esp-idf   (esp-matter 1.4.2 expects IDF v5.4.x)
#   esp-matter  : ~/esp/esp-matter-1.4.2 (export.sh is already sourced by ~/.bashrc)
set -euo pipefail

TARGET="${1:-esp32c3}"
IDF_DIR="${IDF_DIR:-$HOME/esp/v5.4.2/esp-idf}"
MATTER_DIR="${MATTER_DIR:-$HOME/esp/esp-matter-1.4.2}"
PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../firmware" && pwd)"

# shellcheck disable=SC1091
source "$IDF_DIR/export.sh" >/dev/null
export ESP_MATTER_PATH="$MATTER_DIR"
# shellcheck disable=SC1091
source "$MATTER_DIR/export.sh" >/dev/null

cd "$PROJECT_DIR"
BUILD_DIR="build.$TARGET"
SDKCONFIG="sdkconfig.$TARGET"
DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.$TARGET"
# Optional local overrides (Wi-Fi credentials, panel password) - gitignored.
if [ -f "sdkconfig.local" ]; then
    DEFAULTS="$DEFAULTS;sdkconfig.local"
    echo "== using sdkconfig.local overrides"
fi

echo "== target=$TARGET build_dir=$BUILD_DIR"
idf.py -B "$BUILD_DIR" -D SDKCONFIG="$SDKCONFIG" -D SDKCONFIG_DEFAULTS="$DEFAULTS" set-target "$TARGET"
idf.py -B "$BUILD_DIR" -D SDKCONFIG="$SDKCONFIG" -D SDKCONFIG_DEFAULTS="$DEFAULTS" build

echo
echo "== binaries in $PROJECT_DIR/$BUILD_DIR"
echo "   flash from Windows: scripts\\flash-win.bat COM5 $TARGET"
