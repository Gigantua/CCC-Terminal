#!/bin/sh
set -e
cd "$(dirname "$0")"
VER=${1:?version}; ID=${2:-}; KEY=${3:-}; KID=${4:-}; ISS=${5:-}
BUNDLE_ID=${BUNDLE_ID:-com.gigantua.ccc}
APP=bin/ccc.app
OUT=bin/ccc-$VER-macos.dmg
rm -rf "$APP" bin/dmg "$OUT"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
cp bin/ccc "$APP/Contents/MacOS/ccc"
cp icons/ccc.icns "$APP/Contents/Resources/ccc.icns"
cat > "$APP/Contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleName</key><string>ccc</string>
    <key>CFBundleDisplayName</key><string>ccc</string>
    <key>CFBundleIdentifier</key><string>$BUNDLE_ID</string>
    <key>CFBundleExecutable</key><string>ccc</string>
    <key>CFBundleIconFile</key><string>ccc</string>
    <key>CFBundlePackageType</key><string>APPL</string>
    <key>CFBundleShortVersionString</key><string>$VER</string>
    <key>CFBundleVersion</key><string>$VER</string>
    <key>LSMinimumSystemVersion</key><string>13.0</string>
    <key>LSApplicationCategoryType</key><string>public.app-category.developer-tools</string>
    <key>NSHighResolutionCapable</key><true/>
    <key>NSSupportsAutomaticGraphicsSwitching</key><true/>
</dict>
</plist>
EOF
if [ -z "$ID" ]; then
    codesign --force -s - "$APP"
    exit 0
fi
codesign --force --timestamp --options runtime -s "$ID" "$APP"
codesign --verify --strict "$APP"
mkdir -p bin/dmg
cp -R "$APP" bin/dmg/
ln -s /Applications bin/dmg/Applications
hdiutil create -volname "ccc $VER" -srcfolder bin/dmg -format UDZO -ov "$OUT" > /dev/null
rm -rf bin/dmg
codesign --force --timestamp -s "$ID" "$OUT"
[ -n "$KEY" ] || exit 0
xcrun notarytool submit "$OUT" --key "$KEY" --key-id "$KID" --issuer "$ISS" --wait --timeout 60m --no-s3-acceleration | tee bin/notary.log
grep -q 'status: Accepted' bin/notary.log || { xcrun notarytool log "$(grep -m1 '  id:' bin/notary.log | awk '{print $2}')" --key "$KEY" --key-id "$KID" --issuer "$ISS" || true; exit 2; }
for i in 1 2 3 4 5 6 7 8 9 10; do xcrun stapler staple "$OUT" && break; [ $i = 10 ] && exit 3; sleep 30; done
xcrun stapler validate "$OUT"
spctl -a -t open --context context:primary-signature -v "$OUT"
