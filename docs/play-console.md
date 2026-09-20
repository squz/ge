# Play Console — first session for a ge consumer

Generic human-owned Console process. Games keep only identity, listing
copy, SKUs, and asset files in their own repo. Agents prepare those;
they do not create the app, invent SKUs, set prices, or submit
questionnaires without explicit approval.

Companion docs:

- Upload key / AAB / Play App Signing: [`android-release.md`](android-release.md)
- License testers: [`iap-testing.md`](iap-testing.md)
- iOS ship setup: [`release-setup.md`](release-setup.md)

Organisation-agnostic. ge documents the Console *process*. The Play
developer account, package prefix, and keychain *account* (organisation
slug) are caller metadata — they do not live in ge. The upload private
key lives in the macOS keychain, not on the filesystem.

## Account type

Play Console → account settings. Note **personal** vs **organisation**.

- **Organisation** — Internal testing can promote toward production
  without a closed-test clock.
- **Personal, created after 13 Nov 2023** — production stays locked
  until a **closed** test has ≥12 testers opted in continuously for
  14 days
  ([current policy](https://support.google.com/googleplay/android-developer/answer/14151465)).
  Start that clock as soon as the listing + AAB exist. Internal
  testing does **not** count.

## Create the app

1. https://play.google.com/console → **Create app**
2. Name, **package name** (`APP_ID` / `applicationId`), default
   language, type (App or Game), free vs paid.
3. **Check availability** on the package name, then accept the
   declarations.
4. Package name is chosen **here** (Console create form, 2026-08).
   It must match the game's `APP_ID` / `ge/android-init` value and
   `android/app/build.gradle` `applicationId`. Older docs said it
   locked only on the first AAB — that is no longer the create flow.

Record the Play Console **app ID** (the numeric id in the dashboard
URL) and the developer account ID in the game's overlay doc.

## Upload key + first Internal Testing AAB

See [`android-release.md`](android-release.md) for keystore mint,
`make ge/android-bundle`, and Play App Signing enrollment.

Then in Console:

1. **Setup → App signing** — let Google manage the app signing key.
2. **Testing → Internal testing → Create new release** — upload the
   signed AAB.
3. Confirm the upload-key SHA-256 matches `keytool -list -v`.
4. **Start rollout to Internal testing.** Add engineer Google accounts
   to **Setup → License testing** *before* anyone installs the opt-in
   build ([`iap-testing.md`](iap-testing.md)).

## Store listing

Play-required assets (game supplies the files):

| Field | Spec |
|---|---|
| Hi-res icon | 512×512, 32-bit PNG with alpha, ≤1 MB. Full square — Play rounds corners. |
| Feature graphic | 1024×500, JPEG or 24-bit PNG, no alpha. |
| Phone screenshots | ≥2, ≤8. 9:16 or 16:9; 320–3840 px/side; long side ≤2× short. Recommended 1080×1920. JPEG or 24-bit PNG, no alpha. |

Also required to finish the listing: short description (80 chars),
full description, category, contact email, and a **live HTTPS privacy
policy URL**.

## Monetization catalogue

**Monetize → Products → In-app products.** Create one **Managed
product** (non-consumable, subscription, …) per SKU the game already
registered with `ge::iap::setCatalogue`. Product ID must match
**exactly** — do not invent new IDs.

A merchant / payments profile must be complete or purchases fail at
runtime even when products look active.

BillingClient returns "Item not found" until **at least one AAB is
on Internal testing** *and* the products are active.

License testers: [`iap-testing.md`](iap-testing.md).

## Compliance questionnaires

Complete with no blocking incomplete-section warnings:

- Content rating
- Target audience
- Data safety (Play Billing purchase tokens may need an explicit row)
- News / data / ads declarations as they appear

Play Billing purchase history may need a Data safety row even when
the game stores no account of its own. Do not claim "no data
collected" if the form lists Billing as a collected type.

## What agents will not do

- Generate or overwrite a production upload keystore
- Store passwords
- Create the Play app, enroll App Signing, or upload an AAB
- Register SKUs or set prices
- Submit listing / rating / Data safety forms
- Recruit a closed-test panel
