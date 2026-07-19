#!/bin/bash
# Build OrientProbe.apk without gradle: aapt2 + javac + d8 + apksigner.
set -euo pipefail
cd "$(dirname "$0")"

SDK="$HOME/Library/Android/sdk"
BT="$SDK/build-tools/36.0.0"
PLATFORM="$SDK/platforms/android-36/android.jar"

rm -rf out; mkdir -p out/classes

"$BT/aapt2" link --manifest AndroidManifest.xml -I "$PLATFORM" -o out/base.apk
javac -source 17 -target 17 -classpath "$PLATFORM" -d out/classes Probe.java
"$BT/d8" --release --lib "$PLATFORM" --output out out/classes/com/squz/orientprobe/*.class
(cd out && zip -q base.apk classes.dex)
"$BT/zipalign" -f 4 out/base.apk out/aligned.apk
"$BT/apksigner" sign --ks "$HOME/.android/debug.keystore" --ks-pass pass:android \
    --ks-key-alias androiddebugkey --out out/OrientProbe.apk out/aligned.apk
echo "built out/OrientProbe.apk"
