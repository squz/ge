# Android release — upload key, AAB, Play App Signing

Organisation-agnostic process for ge consumers. Debug installs keep
using `assembleDebug` / the Android debug keystore. Release packaging
(`bundleRelease` / `assembleRelease`) requires an upload key and
**never** falls back to debug signing.

The upload private key lives in the **macOS keychain**, not on the
filesystem. ge names the keychain *service* (`play-upload` — the kind
of secret). The *account* is the organisation slug and is caller
metadata (`ANDROID_KEYSTORE_KEYCHAIN_ACCOUNT`, typically `STUDIO`).

Gradle cannot read the keychain. `make ge/android-bundle` exports a
0600 temp PKCS12, runs Gradle, and wipes the file. CI has no login
keychain: it injects a one-shot temp file from a runner secret.

Console first-session (create app, listing, IAP, compliance):
[`play-console.md`](play-console.md).

## Quick path (laptop)

```sh
# 1. One-time per Play organisation: mint the upload key into the
#    keychain. One private key is typically shared by every app on
#    that Play account; Play registers the upload *certificate* per
#    app on first AAB. The PKCS12 password is generated and stored
#    in the same keychain item — do not write a .jks to disk.
ANDROID_KEYSTORE_KEYCHAIN_ACCOUNT=<studio> \
  ./ge/tools/generate-android-upload-keystore.sh

# 2. Build the signed App Bundle (exports a temp PKCS12, then wipes it)
ANDROID_KEYSTORE_KEYCHAIN_ACCOUNT=<studio> \
  make ge/android-bundle
# → android/app/build/outputs/bundle/release/app-release.aab
```

Keychain lookup:

| Field | Value | Who chooses |
|---|---|---|
| Service | `play-upload` (override `ANDROID_KEYSTORE_KEYCHAIN_SERVICE`) | ge (kind of secret) |
| Account | organisation slug | game `STUDIO` / `ANDROID_KEYSTORE_KEYCHAIN_ACCOUNT` |

## Versioning

| Field | Source | Notes |
|---|---|---|
| `versionName` | `android/version.properties` or `VERSION_NAME` | Marketing version; align with iOS unless deliberately forked |
| `versionCode` | `android/version.properties` or `VERSION_CODE` | Integer, **monotonically non-decreasing** across Play uploads |

Do **not** derive `versionCode` from `git rev-list --count` — squash
merges decrease that count.

Games may ship `make android-bump-version` (or equivalent) that
increments the committed counter and commits nothing.

## Secrets policy

| Item | Location | In git? |
|---|---|---|
| Upload private key | macOS keychain (`play-upload` / `<studio>`) | **Never** |
| PKCS12 password | same keychain item (generated at mint) | **Never** |
| Persistent `.jks` / `.keystore` | **do not keep one** | **Never** |
| `android/keystore.properties` | CI / legacy fallback only | **Never** (gitignored) |
| `android/keystore.properties.example` | documents CI env names | Yes |
| `android/version.properties` | marketing + counter | Yes |

`android/.gitignore` must ignore `keystore.properties`, `*.jks`,
`*.keystore`.

## CI injection

CI should **not** commit a keystore and usually has no login keychain.
Decode a runner secret to a temp file, point env at it, wipe after.

| Secret / env | Description |
|---|---|
| `ANDROID_KEYSTORE_PATH` | Absolute path to a **temp** PKCS12 on the runner |
| `ANDROID_KEYSTORE_PASSWORD` | Keystore password (`STORE_PASSWORD` also accepted) |
| `ANDROID_KEY_ALIAS` | Key alias, typically `upload` (`KEY_ALIAS` also accepted) |
| `ANDROID_KEY_PASSWORD` | Key password (`KEY_PASSWORD` also accepted) |

```yaml
- name: Write upload keystore
  run: |
    install -m 600 /dev/null "$RUNNER_TEMP/play-upload.p12"
    echo "${{ secrets.ANDROID_KEYSTORE_BASE64 }}" | base64 -d \
      > "$RUNNER_TEMP/play-upload.p12"
- name: Bundle release
  env:
    ANDROID_KEYSTORE_PATH: ${{ env.RUNNER_TEMP }}/play-upload.p12
    ANDROID_KEYSTORE_PASSWORD: ${{ secrets.ANDROID_KEYSTORE_PASSWORD }}
    ANDROID_KEY_ALIAS: upload
    ANDROID_KEY_PASSWORD: ${{ secrets.ANDROID_KEY_PASSWORD }}
  run: make ge/android-bundle
```

If `VERSION_CODE` comes from `github.run_number`, ensure it never goes
backwards relative to what is already on Play.

`PLAY_SERVICE_ACCOUNT_JSON` (see [`release-setup.md`](release-setup.md))
uploads the AAB after it is signed. The upload-key secrets above are
additional.

To seed the CI secret from a laptop keychain item:

```sh
eval "$(ANDROID_KEYSTORE_KEYCHAIN_ACCOUNT=<studio> \
  ./ge/tools/android-upload-keychain.sh export)"
base64 -i "$ANDROID_KEYSTORE_PATH" | pbcopy
# paste into ANDROID_KEYSTORE_BASE64; password is $ANDROID_KEYSTORE_PASSWORD
rm -P "$ANDROID_KEYSTORE_PATH"
```

## Make / fastlane entry points

| Command | Result |
|---|---|
| `make ge/android` | `assembleDebug` (debug keystore — local/CI cells) |
| `make ge/android-release` | `assembleRelease` APK (unsigned unless secrets present) |
| `make ge/android-bundle` | `:app:bundleRelease` → signed AAB (requires upload key) |

Output path:

```
android/app/build/outputs/bundle/release/app-release.aab
```

## Play App Signing — human enrollment checklist

Agents cannot enroll Play Console apps. A human with Console access
must complete this once per app.

### Prerequisites

- [ ] Play Console access for the account that will own the package.
- [ ] Upload key in the keychain:
      `ANDROID_KEYSTORE_KEYCHAIN_ACCOUNT=<studio> ./ge/tools/android-upload-keychain.sh status`
- [ ] At least one signed AAB: `ANDROID_KEYSTORE_KEYCHAIN_ACCOUNT=<studio> make ge/android-bundle`.

### Enroll Play App Signing (mandatory for new apps)

1. Play Console → app → **Setup → App signing**
   (or first-upload wizard: "Google Play App Signing").
2. Choose **Let Google manage and protect your app signing key**.
3. On first AAB upload, Play registers:
   - **App signing key** — generated and held by Google; used to sign
     what users install. You never hold this private key.
   - **Upload key** — the keychain item; used only to sign AABs you
     upload. Play re-signs with the app signing key after accepting
     the upload.
4. Confirm the upload-key certificate fingerprint shown in Console
   matches:
   `ANDROID_KEYSTORE_KEYCHAIN_ACCOUNT=<studio> ./ge/tools/android-upload-keychain.sh fingerprint`

### Record the two certificates

Keep the upload-key fingerprints with the keychain item (Keychain
Access comment, or a note in the same item). Copy the Play-managed
app-signing fingerprints from Console. Upload key **≠** app signing
key (by design).

### First Internal Testing upload

1. Play Console → **Testing → Internal testing → Create new release**.
2. Upload `android/app/build/outputs/bundle/release/app-release.aab`.
3. If Play App Signing prompts, accept Google-managed signing and
   verify fingerprints.
4. Set release notes, review, **Start rollout to Internal testing**.
5. Add license testers *before* anyone installs
   ([`iap-testing.md`](iap-testing.md)).

### Upload-key loss / rotation

1. Play Console → App signing → **Request upload key reset**.
2. Mint a **new** key:
   `ANDROID_KEYSTORE_KEYCHAIN_REPLACE=1 ANDROID_KEYSTORE_KEYCHAIN_ACCOUNT=<studio> ./ge/tools/generate-android-upload-keystore.sh`
3. Submit the new certificate PEM to Play; after approval, refresh
   any CI copy of the PKCS12.
4. The Play-managed **app signing key** is unchanged — users are
   unaffected.

## Verify an AAB locally

```sh
jarsigner -verify -verbose -certs \
  android/app/build/outputs/bundle/release/app-release.aab | head -40

# brew install bundletool
bundletool dump manifest \
  --bundle android/app/build/outputs/bundle/release/app-release.aab \
  | grep -E 'package=|versionCode|versionName'
```

Expect the game's `applicationId`, a `versionName` matching
`android/version.properties`, and a signature from the upload cert
(not the Android debug cert).
