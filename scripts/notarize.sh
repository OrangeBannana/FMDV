#!/bin/sh
# Sign-off step: notarize the release artifacts so Gatekeeper opens FMDV without
# warnings on machines other than the build host. Produces a notarized + stapled
# build/FMDV-macos.zip (the in-app updater's payload) AND build/FMDV-macos.dmg
# (the drag-to-Applications download).
#
# Run after building a Developer ID-signed app:
#   make dist FMDV_SIGN_ID="Developer ID Application: Name (TEAMID)"
#   scripts/notarize.sh
# Notarization requires a Developer ID signature with hardened runtime (which
# `make app`/`make dist` produce when FMDV_SIGN_ID is set); an ad-hoc build is
# rejected after upload.
#
# One-time setup (needs an Apple Developer Program membership):
#   1. Create a "Developer ID Application" certificate at
#      https://developer.apple.com/account/resources/certificates and install
#      it in your login keychain. ("Apple Development"/"Apple Distribution"
#      certs won't do — those are for local dev and the App Store.)
#   2. Store notarytool credentials under a keychain profile (uses an
#      app-specific password from https://account.apple.com, not your real
#      Apple ID password):
#        xcrun notarytool store-credentials fmdv-notary \
#          --apple-id you@example.com --team-id YOURTEAMID
#
# Usage: scripts/notarize.sh [keychain-profile]   (default: fmdv-notary)
set -eu

APP=build/FMDV.app
ZIP=build/FMDV-macos.zip
DMG=build/FMDV-macos.dmg
PROFILE=${1:-fmdv-notary}

[ -d "$APP" ] || { echo "no $APP — run: make dist FMDV_SIGN_ID=\"Developer ID Application: ...\"" >&2; exit 1; }

# Refuse to submit an ad-hoc build: notarization would fail after the upload.
# The same Developer ID identity is reused to sign the .dmg below.
SIGN_ID=$(codesign -dvv "$APP" 2>&1 | sed -n 's/^Authority=\(Developer ID Application:.*\)$/\1/p' | head -1)
[ -n "$SIGN_ID" ] || { echo "$APP is not Developer ID-signed — rebuild with FMDV_SIGN_ID set" >&2; exit 1; }

# --- 1. app + zip: a .zip can't be stapled, so notarize it, staple the ticket
#        to the .app inside, then re-zip so the payload carries a stapled app. ---
[ -f "$ZIP" ] || ditto -c -k --keepParent "$APP" "$ZIP"
echo "== submitting $ZIP for notarization (profile: $PROFILE) =="
xcrun notarytool submit "$ZIP" --keychain-profile "$PROFILE" --wait
echo "== stapling ticket to $APP =="
xcrun stapler staple "$APP"
rm -f "$ZIP"
ditto -c -k --keepParent "$APP" "$ZIP"

# --- 2. dmg: build the drag-installer from the now-stapled app, sign the image
#        with the same Developer ID, notarize it, and staple the ticket directly
#        to the .dmg. Built here (not via `make dmg`) so the enclosed app is the
#        stapled one and `make`'s $(APP) rule doesn't rebuild over the staple. ---
VER=$(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' "$APP/Contents/Info.plist")
STAGE=$(mktemp -d "${TMPDIR:-/tmp}/fmdv-dmg.XXXXXX")
cp -R "$APP" "$STAGE/"
ln -s /Applications "$STAGE/Applications"
rm -f "$DMG"
hdiutil create -volname "FMDV $VER" -srcfolder "$STAGE" -fs HFS+ -format UDZO -ov "$DMG" >/dev/null
rm -rf "$STAGE"
codesign --force --sign "$SIGN_ID" --timestamp "$DMG"
echo "== submitting $DMG for notarization =="
xcrun notarytool submit "$DMG" --keychain-profile "$PROFILE" --wait
echo "== stapling ticket to $DMG =="
xcrun stapler staple "$DMG"

echo "== notarized + stapled: $ZIP and $DMG =="
echo "verify: xcrun stapler validate \"$DMG\" && spctl -a -vv \"$APP\""
