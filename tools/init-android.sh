#!/usr/bin/env bash
#
# Generate an Android direct-mode project for a ge app.
#
# Usage:
#   ge/tools/init-android.sh <package> <app-name>
#
# Example:
#   ge/tools/init-android.sh com.squz.mygame "My Game"
#
# Creates $APP_DIR/android/ (where $APP_DIR is the current working directory)
# with a complete Gradle project that builds a standalone APK (direct
# rendering via sokol/Vulkan).

set -euo pipefail

if [ $# -lt 2 ]; then
    echo "Usage: $0 <package> <app-name>"
    echo "  package   — Java package / application ID (e.g. com.squz.mygame)"
    echo "  app-name  — display name shown on the device (e.g. \"My Game\")"
    exit 1
fi

PACKAGE="$1"
APP_NAME="$2"

# Derive names from app name (strip spaces for class/project names).
STRIPPED="$(echo "$APP_NAME" | tr -d ' ')"
CMAKE_PROJECT="$STRIPPED"
# As of 🎯T41 (v0.10.0+), the Activity is canonical-engine-owned —
# AndroidManifest.xml hardcodes android:name="ge.GeActivity" and
# Gradle source-includes it from $GE_ROOT/android-shared. Apps no
# longer scaffold a per-app Activity.java. Apps that need custom
# behavior subclass ge.GeActivity in their own java tree.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
TEMPLATE_DIR="$SCRIPT_DIR/android-template"
GE_ROOT_ABS="$(cd "$SCRIPT_DIR/.." && pwd)"
OUT_DIR="$(pwd)/android"

# GE_ROOT reference (relative to $OUT_DIR) so the template works whether ge
# is a submodule or the consumer is an in-tree sample.
GE_REL="$(python3 -c "import os,sys; print(os.path.relpath(sys.argv[1], sys.argv[2]))" \
    "$GE_ROOT_ABS" "$OUT_DIR")"

if [ -d "$OUT_DIR" ]; then
    echo "Error: $OUT_DIR already exists. Remove it first to regenerate."
    exit 1
fi

echo "Generating Android project:"
echo "  Package:  $PACKAGE"
echo "  App name: $APP_NAME"
echo "  Activity: ge.GeActivity (canonical, source-included from \$GE/android-shared)"
echo "  Output:   $OUT_DIR"
echo "  ge:       $GE_REL (from android/)"
echo

# Static file copy.
mkdir -p "$OUT_DIR"
cp -r "$TEMPLATE_DIR/gradle"       "$OUT_DIR/"
cp     "$TEMPLATE_DIR/gradlew"     "$OUT_DIR/"
cp     "$TEMPLATE_DIR/gradlew.bat" "$OUT_DIR/" 2>/dev/null || true
chmod +x "$OUT_DIR/gradlew"
cp "$TEMPLATE_DIR/build.gradle"       "$OUT_DIR/"
cp "$TEMPLATE_DIR/settings.gradle"    "$OUT_DIR/" 2>/dev/null || true
cp "$TEMPLATE_DIR/gradle.properties"  "$OUT_DIR/"
cp "$TEMPLATE_DIR/.gitignore"         "$OUT_DIR/" 2>/dev/null || true

# Templated file generation.
subst() {
    sed -e "s|__PACKAGE__|$PACKAGE|g" \
        -e "s|__APP_NAME__|$APP_NAME|g" \
        -e "s|__CMAKE_PROJECT__|$CMAKE_PROJECT|g" \
        -e "s|__GE_REL__|$GE_REL|g" \
        "$1"
}

mkdir -p "$OUT_DIR/app"
subst "$TEMPLATE_DIR/app/build.gradle.in" > "$OUT_DIR/app/build.gradle"

mkdir -p "$OUT_DIR/app/src/main/cpp"
subst "$TEMPLATE_DIR/app/src/main/cpp/CMakeLists.txt.in" > "$OUT_DIR/app/src/main/cpp/CMakeLists.txt"

mkdir -p "$OUT_DIR/app/src/main"
subst "$TEMPLATE_DIR/app/src/main/AndroidManifest.xml.in" > "$OUT_DIR/app/src/main/AndroidManifest.xml"

mkdir -p "$OUT_DIR/app/src/main/res/values"
subst "$TEMPLATE_DIR/app/src/main/res/values/strings.xml.in" > "$OUT_DIR/app/src/main/res/values/strings.xml"

# No per-app Activity.java is scaffolded — AndroidManifest.xml binds
# directly to ge.GeActivity, which Gradle source-includes from
# $GE_ROOT/android-shared/src/main/java (see app/build.gradle).

# Auto-detect Android SDK.
if [ -d "$HOME/Library/Android/sdk" ]; then
    echo "sdk.dir=$HOME/Library/Android/sdk" > "$OUT_DIR/local.properties"
elif [ -n "${ANDROID_HOME:-}" ]; then
    echo "sdk.dir=$ANDROID_HOME" > "$OUT_DIR/local.properties"
fi

echo "Done. Build with:"
echo "  make ge/android"
