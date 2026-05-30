#!/usr/bin/env bash
#
# Generate an iOS direct-mode project for a ge app.
#
# Usage:
#   ge/tools/init-ios.sh <bundle-id> <app-name> [dev-team]
#
# Example:
#   ge/tools/init-ios.sh com.squz.mygame "My Game" SWA3H3N7TW
#
# Creates $APP_DIR/ios/ with project.rb (xcodeproj-gem builder driver,
# 🎯T73.2 — replaces the old CMakeLists.txt path) and Info.plist, where
# $APP_DIR is the current working directory (the consuming app's root).
# Generate the Xcode project and build with: make ge/ios

set -euo pipefail

if [ $# -lt 2 ]; then
    echo "Usage: $0 <bundle-id> <app-name> [dev-team]"
    echo "  bundle-id — iOS bundle identifier (e.g. com.squz.mygame)"
    echo "  app-name  — display name shown on the device (e.g. \"My Game\")"
    echo "  dev-team  — Xcode development team id (optional)"
    exit 1
fi

BUNDLE_ID="$1"
APP_NAME="$2"
DEVELOPMENT_TEAM="${3:-}"

# Target / binary name: app name with spaces stripped (mirrors the
# build_project.rb `app_name` kwarg used as the xcodeproj target name).
APP_BIN_NAME="$(echo "$APP_NAME" | tr -d ' ')"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
TEMPLATE_DIR="$SCRIPT_DIR/ios-template"
GE_ROOT_ABS="$(cd "$SCRIPT_DIR/.." && pwd)"
OUT_DIR="$(pwd)/ios"

# GE_ROOT reference (relative to $OUT_DIR) so the template works whether ge
# is a submodule (GE_REL=../ge) or the consumer is an in-tree sample
# (GE_REL=../../..).
GE_REL="$(python3 -c "import os,sys; print(os.path.relpath(sys.argv[1], sys.argv[2]))" \
    "$GE_ROOT_ABS" "$OUT_DIR")"

if [ -d "$OUT_DIR" ]; then
    echo "Error: $OUT_DIR already exists. Remove it first to regenerate."
    exit 1
fi

echo "Generating iOS project:"
echo "  Bundle ID: $BUNDLE_ID"
echo "  App name:  $APP_NAME"
echo "  Target:    $APP_BIN_NAME"
echo "  Output:    $OUT_DIR"
echo "  ge:        $GE_REL (from ios/)"
echo

mkdir -p "$OUT_DIR"

subst() {
    sed -e "s|__BUNDLE_ID__|$BUNDLE_ID|g" \
        -e "s|__APP_NAME__|$APP_NAME|g" \
        -e "s|__APP_BIN_NAME__|$APP_BIN_NAME|g" \
        -e "s|__DEVELOPMENT_TEAM__|$DEVELOPMENT_TEAM|g" \
        -e "s|__GE_REL__|$GE_REL|g" \
        "$1"
}

subst "$TEMPLATE_DIR/project.rb.in" > "$OUT_DIR/project.rb"
subst "$TEMPLATE_DIR/Info.plist.in" > "$OUT_DIR/Info.plist"

echo "Done. Generate and build with:"
echo "  make ge/ios"
