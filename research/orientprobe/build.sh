#!/bin/bash
# Build OrientProbe.app variants for the iOS simulator.
# Variants differ ONLY in Info.plist packaging; runtime behavior comes from env vars.
set -euo pipefail
cd "$(dirname "$0")"

SDK=iphonesimulator
MIN=16.0
xcrun -sdk $SDK swiftc -target arm64-apple-ios${MIN}-simulator -O main.swift -o Probe
xcrun -sdk $SDK swiftc -target arm64-apple-ios26.0-simulator -D PROBE26 -O main.swift -o Probe26

plist_orient() {
  case "$1" in
    land) echo '<string>UIInterfaceOrientationLandscapeLeft</string><string>UIInterfaceOrientationLandscapeRight</string>' ;;
    port) echo '<string>UIInterfaceOrientationPortrait</string><string>UIInterfaceOrientationPortraitUpsideDown</string>' ;;
    all)  echo '<string>UIInterfaceOrientationPortrait</string><string>UIInterfaceOrientationPortraitUpsideDown</string><string>UIInterfaceOrientationLandscapeLeft</string><string>UIInterfaceOrientationLandscapeRight</string>' ;;
  esac
}

# make_variant <name> <orient-set> <extra-plist-xml> [binary]
make_variant() {
  local name="$1" orient="$2" extra="${3:-}" bin="${4:-Probe}"
  local app="build/${name}.app"
  rm -rf "$app"; mkdir -p "$app"
  cp "$bin" "$app/$name"
  cat > "$app/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleIdentifier</key><string>com.squz.orientprobe.${name}</string>
<key>CFBundleName</key><string>${name}</string>
<key>CFBundleDisplayName</key><string>${name}</string>
<key>CFBundleExecutable</key><string>${name}</string>
<key>CFBundleVersion</key><string>1</string>
<key>CFBundleShortVersionString</key><string>1.0</string>
<key>CFBundlePackageType</key><string>APPL</string>
<key>LSRequiresIPhoneOS</key><true/>
<key>MinimumOSVersion</key><string>${MIN}</string>
<key>UIDeviceFamily</key><array><integer>1</integer><integer>2</integer></array>
<key>UILaunchScreen</key><dict/>
<key>UISupportedInterfaceOrientations</key><array>$(plist_orient "$orient")</array>
${extra}
</dict></plist>
EOF
  echo "built $app"
}

mkdir -p build
make_variant ProbeLand land
make_variant ProbePort port
make_variant ProbeAll  all
make_variant ProbeLandRFS land '<key>UIRequiresFullScreen</key><true/>'
make_variant ProbeLandDRC land '<key>UIDesignRequiresCompatibility</key><true/>'
make_variant ProbeAll26  all '' Probe26
make_variant ProbeLand26 land '' Probe26
echo DONE
