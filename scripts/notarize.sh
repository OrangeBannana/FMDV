#!/bin/sh
# Notarize build/FMDV-macos.zip so Gatekeeper opens the app without warnings
# on other machines. Run after `make dist FMDV_SIGN_ID="Developer ID
# Application: ..."` — notarization requires a Developer ID signature with
# hardened runtime (which that produces); an ad-hoc build will be rejected.
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

ZIP=build/FMDV-macos.zip
APP=build/FMDV.app
PROFILE=${1:-fmdv-notary}

[ -f "$ZIP" ] || { echo "no $ZIP — run: make dist FMDV_SIGN_ID=\"Developer ID Application: ...\"" >&2; exit 1; }

# Refuse to submit an ad-hoc build: notarization would fail after the upload.
if ! codesign -dv "$APP" 2>&1 | grep -q "Authority=Developer ID Application"; then
    echo "$APP is not Developer ID-signed — rebuild with FMDV_SIGN_ID set" >&2
    exit 1
fi

echo "== submitting $ZIP for notarization (profile: $PROFILE) =="
xcrun notarytool submit "$ZIP" --keychain-profile "$PROFILE" --wait

# Staple the ticket to the .app (zips can't be stapled), then re-zip so the
# release artifact passes Gatekeeper even offline.
echo "== stapling ticket to $APP =="
xcrun stapler staple "$APP"
rm -f "$ZIP"
ditto -c -k --keepParent "$APP" "$ZIP"
echo "== notarized + stapled: $ZIP =="
