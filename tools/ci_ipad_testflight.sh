#!/usr/bin/env bash
# ============================================================================
#  tools/ci_ipad_testflight.sh -- signed iPad archive, exported for App Store
#  Connect and uploaded to TestFlight. Runs in the ipad job of
#  .github/workflows/build.yml, after build-ios/ has been configured.
#
#  Adapted from PulseRoom's scripts/ios-build.sh, which reached TestFlight on
#  2026-09-12. The key points it established:
#    - Only an "Apple Distribution" identity can sign an App Store build.
#    - With no provisioning-profile secret, Xcode creates the distribution
#      profile for the bundle id itself, through the App Store Connect API key
#      (-allowProvisioningUpdates). CODE_SIGN_IDENTITY must say "Apple
#      Distribution" explicitly, or Xcode asks for a DEVELOPMENT profile,
#      which Apple refuses for a team with no registered devices.
#
#  Secret values are never printed -- only names and outcomes.
# ============================================================================
set -euo pipefail

WORK="${GITHUB_WORKSPACE:-$(pwd)}"
TMP="${RUNNER_TEMP:-/tmp}"
BUNDLE_ID="com.amanorsac.performlive"
VERSION="$(sed -n 's/^project(EzPlay VERSION \([0-9.]*\)).*/\1/p' "$WORK/CMakeLists.txt")"
BUILD_NUMBER="${GITHUB_RUN_NUMBER:-1}"
IPA_OUT="$WORK/PerformLive-$VERSION-iPad.ipa"

say_output() { echo "$1" >> "${GITHUB_OUTPUT:-/dev/null}"; }
trim() { printf '%s' "$1" | sed -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//'; }

# ---- what do we have ---------------------------------------------------------
missing=()
[ -n "${APPLE_TEAM_ID:-}" ] || missing+=(APPLE_TEAM_ID)
[ -n "${ASC_KEY_ID:-}" ]    || missing+=(ASC_KEY_ID)
[ -n "${ASC_ISSUER_ID:-}" ] || missing+=(ASC_ISSUER_ID)
[ -n "${ASC_KEY_P8:-}" ]    || missing+=(ASC_KEY_P8)
[ -n "${DIST_P12_A:-}${DIST_P12_B:-}" ] || missing+=("IOS_DIST_CERT_P12 or APPLE_DIST_P12")
if [ ${#missing[@]} -gt 0 ]; then
  printf 'Missing secret: %s\n' "${missing[@]}"
  echo "::error::The iPad build was not signed, so nothing went to TestFlight. Missing: ${missing[*]}"
  say_output "signed=false"
  exit 1
fi

# ---- the Apple Distribution identity -----------------------------------------
KEYCHAIN="$TMP/ipad-signing.keychain-db"
KEYCHAIN_PASSWORD="$(uuidgen)"
security delete-keychain "$KEYCHAIN" >/dev/null 2>&1 || true
security create-keychain -p "$KEYCHAIN_PASSWORD" "$KEYCHAIN"
security set-keychain-settings -lut 21600 "$KEYCHAIN"
security unlock-keychain -p "$KEYCHAIN_PASSWORD" "$KEYCHAIN"
security list-keychains -d user -s "$KEYCHAIN" $(security list-keychains -d user | tr -d '"')
trap 'security delete-keychain "$KEYCHAIN" >/dev/null 2>&1 || true; rm -f "$TMP/dist.p12"' EXIT

import_p12() {   # $1 = secret name (for the log), $2 = base64 value
  [ -n "$2" ] || return 1
  if ! printf '%s' "$2" | tr -d '[:space:]' | base64 --decode > "$TMP/dist.p12" 2>/dev/null || [ ! -s "$TMP/dist.p12" ]; then
    echo "  $1: not valid base64"; return 1
  fi
  local pw
  for pw in "${DIST_PW_A:-}" "${DIST_PW_B:-}" "${DIST_PW_C:-}" ""; do
    if security import "$TMP/dist.p12" -k "$KEYCHAIN" -P "$(trim "$pw")" \
         -T /usr/bin/codesign -T /usr/bin/security >/dev/null 2>&1; then
      echo "  $1: imported"; rm -f "$TMP/dist.p12"; return 0
    fi
  done
  echo "  $1: could not be opened with any of the password secrets"
  rm -f "$TMP/dist.p12"; return 1
}

echo "Distribution certificate secrets:"
import_p12 IOS_DIST_CERT_P12 "${DIST_P12_A:-}" || true
import_p12 APPLE_DIST_P12    "${DIST_P12_B:-}" || true
security set-key-partition-list -S apple-tool:,apple:,codesign: -s -k "$KEYCHAIN_PASSWORD" "$KEYCHAIN" >/dev/null

echo "Code-signing identities:"
security find-identity -v -p codesigning "$KEYCHAIN" | sed 's/^/  /'
IDENTITY="$(security find-identity -v -p codesigning "$KEYCHAIN" | grep -o -E '"(Apple|iPhone) Distribution[^"]*"' | head -1 | tr -d '"' || true)"
if [ -z "$IDENTITY" ]; then
  echo "::error::No Apple Distribution identity could be imported. An App Store build needs the Apple Distribution certificate WITH its private key (Developer ID and Mac installer certificates cannot sign iPad apps)."
  say_output "signed=false"
  exit 1
fi
echo "Signing as: $IDENTITY"

# ---- App Store Connect API key ----------------------------------------------
KEY_DIR="$HOME/.appstoreconnect/private_keys"
mkdir -p "$KEY_DIR"
KEY_PATH="$KEY_DIR/AuthKey_$(trim "$ASC_KEY_ID").p8"
printf '%s' "$ASC_KEY_P8" | base64 --decode > "$KEY_PATH" 2>/dev/null || true
grep -q "BEGIN PRIVATE KEY" "$KEY_PATH" 2>/dev/null || printf '%s\n' "$ASC_KEY_P8" > "$KEY_PATH"
grep -q "BEGIN PRIVATE KEY" "$KEY_PATH" || { echo "::error::ASC_KEY_P8 is neither base64 nor a raw .p8 key."; exit 1; }
trap 'security delete-keychain "$KEYCHAIN" >/dev/null 2>&1 || true; rm -f "$TMP/dist.p12" "$KEY_PATH"' EXIT

AUTH=(-allowProvisioningUpdates
      -authenticationKeyPath "$KEY_PATH"
      -authenticationKeyID "$(trim "$ASC_KEY_ID")"
      -authenticationKeyIssuerID "$(trim "$ASC_ISSUER_ID")")

# ---- archive -----------------------------------------------------------------
ARCHIVE="$TMP/PerformLive.xcarchive"
EXPORT_DIR="$TMP/export"
rm -rf "$ARCHIVE" "$EXPORT_DIR"

archive_with() {
  set -o pipefail
  xcodebuild -project "$WORK/build-ios/EzPlay.xcodeproj" -scheme EzPlay -configuration Release \
    -sdk iphoneos -destination 'generic/platform=iOS' \
    -archivePath "$ARCHIVE" archive \
    "${AUTH[@]}" "$@" \
    DEVELOPMENT_TEAM="$(trim "$APPLE_TEAM_ID")" \
    PRODUCT_BUNDLE_IDENTIFIER="$BUNDLE_ID" \
    OTHER_CODE_SIGN_FLAGS="--keychain $KEYCHAIN" \
    2>&1 | tee "$WORK/ipad-archive.log" | grep -E "error:|warning: .*(sign|provision)|ARCHIVE (SUCCEEDED|FAILED)" || true
  [ -d "$ARCHIVE/Products/Applications" ]
}

if ! archive_with CODE_SIGN_STYLE=Automatic CODE_SIGN_IDENTITY="Apple Distribution" PROVISIONING_PROFILE_SPECIFIER=""; then
  # Export only ever asks for a distribution profile, so an unsigned archive
  # signed at export is the fallback that works when archive-time signing won't.
  echo "::notice::Signed archive failed; archiving unsigned and signing at export instead."
  grep -E "error:" "$WORK/ipad-archive.log" | head -20 || true
  rm -rf "$ARCHIVE"
  archive_with CODE_SIGNING_ALLOWED=NO CODE_SIGNING_REQUIRED=NO CODE_SIGN_IDENTITY="" \
    || { echo "::error::The iPad archive failed."; tail -60 "$WORK/ipad-archive.log"; exit 1; }
fi

# ---- export ------------------------------------------------------------------
export_as() {   # $1 = export method
  cat > "$TMP/ExportOptions.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>method</key><string>$1</string>
  <key>teamID</key><string>$(trim "$APPLE_TEAM_ID")</string>
  <key>signingStyle</key><string>automatic</string>
  <key>uploadSymbols</key><true/>
  <key>compileBitcode</key><false/>
</dict></plist>
PLIST
  xcodebuild -exportArchive -archivePath "$ARCHIVE" -exportPath "$EXPORT_DIR" \
    -exportOptionsPlist "$TMP/ExportOptions.plist" "${AUTH[@]}" 2>&1 | tail -25
  ls "$EXPORT_DIR"/*.ipa >/dev/null 2>&1
}

if ! export_as app-store-connect; then
  echo "::notice::Export as 'app-store-connect' failed; retrying with the older name 'app-store'."
  export_as app-store || { echo "::error::Export produced no .ipa."; exit 1; }
fi

mv "$(ls "$EXPORT_DIR"/*.ipa | head -1)" "$IPA_OUT"
echo "Wrote $(basename "$IPA_OUT") (version $VERSION, build $BUILD_NUMBER)"
say_output "signed=true"

rm -rf "$TMP/verify" && mkdir -p "$TMP/verify"
unzip -q "$IPA_OUT" -d "$TMP/verify"
codesign -dv --verbose=2 "$TMP/verify/Payload/PerformLive.app" 2>&1 | sed -n '1,12p'

# ---- TestFlight --------------------------------------------------------------
echo "--- uploading to App Store Connect ---"
set +e
UPLOAD="$(xcrun altool --upload-app -f "$IPA_OUT" -t ios \
            --apiKey "$(trim "$ASC_KEY_ID")" --apiIssuer "$(trim "$ASC_ISSUER_ID")" 2>&1)"
RC=$?
set -e
printf '%s\n' "$UPLOAD" | tail -25
if [ $RC -ne 0 ] || printf '%s' "$UPLOAD" | grep -qiE "ERROR ITMS|UPLOAD FAILED|No suitable application records"; then
  if printf '%s' "$UPLOAD" | grep -qi "No suitable application records"; then
    echo "::error::App Store Connect has no app for $BUNDLE_ID yet. Create it once (App Store Connect > Apps > + > New App, platform iOS, bundle id $BUNDLE_ID) and re-run this job."
  else
    echo "::error::TestFlight upload failed (see Apple's message above)."
  fi
  exit 1
fi
echo "Uploaded build $BUILD_NUMBER. It appears in TestFlight once Apple finishes processing (usually 5-30 minutes)."
