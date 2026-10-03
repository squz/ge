# Release setup (🎯T64.1 — studio-wide signing via fastlane match)

This is the **one-time setup** a new engineer or a fresh machine needs before
running any of the `make ship-*` lanes (🎯T64.2). After this, day-to-day
shipping uses `spyder fastlane --studio squz -- sync_certs` (readonly,
also called by the build lane). Spyder injects credentials into its child only.

> **Studio scope.** ge is studio-agnostic. This document uses the **squz**
> studio as the worked example (cert repo `squz/certs`, Apple Team
> `SWA3H3N7TW`); any studio consuming ge substitutes its own values. The
> `fastlane/Matchfile` with the studio's actual repo URL + team ID lives in
> the **consuming game's repo** (e.g. `multimaze2/fastlane/Matchfile`), not
> in ge. ge ships only `fastlane/Matchfile.example` as a template, plus the
> generic `fastlane/Fastfile` lane definitions.

## Prerequisites you can't avoid

1. **Apple Developer Program membership** on the squz team (`SWA3H3N7TW`).
   Marcelo grants access via App Store Connect → Users & Access → Add User.
2. **GitHub access to `squz/certs`** (private repo holding encrypted
   signing material). Marcelo invites you via the repo's Settings → Collaborators.
3. **1Password access to the squz vault** for the match passphrase.

## Step 1 — App Store Connect API key

1. Log in to https://appstoreconnect.apple.com → Users and Access → Keys (top-right tab).
2. Click **Generate API Key**. Name it after your laptop, e.g. `marcelo-mbp`.
3. Access: **Developer** is the minimum, **App Manager** if you'll be
   uploading TestFlight builds from this machine.
4. Download the `.p8` immediately — Apple lets you download it exactly once.
   Save to `~/.appstoreconnect/private_keys/AuthKey_<KEYID>.p8` (create
   the directory if it doesn't exist).
5. Note the **Key ID** (shown in the row) and the **Issuer ID** (shown
   at the top of the Keys page; same for everyone on the team).

## Step 2 — Studio secrets and app configuration

Install a signed spyder with the [ship front door](https://github.com/marcelocantos/spyder/blob/master/docs/ship-front-door.md)
and follow its interactive import flow on the developer machine:

```sh
spyder secret import --studio squz
spyder secret missing --studio squz --for match
spyder secret missing --studio squz --for pilot
spyder secret missing --studio squz --for deliver
```

The import flow absorbs credentials into the studio keychain envelope. Do not
export `MATCH_PASSWORD` or ASC credentials in shell profiles or Makefiles, or
print secrets to verify them. Remove old exports from shell profiles. ge neither
reads the key nor constructs credential JSON; spyder owns child-only injection.
Missing-secret checks return exit 20 when required material is absent.

Set non-secret app metadata in the consuming Makefile: `SHIP_SCHEME`, `APP_ID`,
`APPLE_TEAM_ID`, and optionally `SHIP_PROJECT`. `STUDIO` defaults to `squz`;
override it with `make ship-alpha STUDIO=your-studio`. The project defaults to
`ios/build/xcode/$(SHIP_SCHEME).xcodeproj` and is passed as a fastlane `project:`
argument because spyder filters the parent environment.

## Step 3 — Ruby + fastlane

```sh
cd ~/work/github.com/squz/<your-game>
bundle install        # reads Gemfile, installs fastlane 2.234.x
```

If `bundle` complains about Ruby version, install the recommended one via
Homebrew: `brew install ruby` and add `/opt/homebrew/opt/ruby/bin` to your
`PATH` ahead of the system Ruby.

## Step 4 — Verify match

```sh
spyder fastlane --studio squz -- sync_certs
# Readonly certificate/profile resolution. No certificate creation or rotation.
```

## Step 4b — Enroll a new test device (🎯T110, optional)

Physical-device **development** installs need the device UDID on the team.
Do this **once per device**, without opening Xcode:

```sh
# Auto-discover connected devices (spyder or xctrace) and register them
# with the Developer Portal via the ASC API key from Step 1–2.
make ge/ios-register-devices

# Then build a Debug, Automatic-signed .app:
make ge/ios-device
# Install via spyder / devicectl as usual.
```

`REGISTER_DEVICES=1 make ge/ios-device` enrolls then builds. Re-running
registration is safe (add-only). Hardware UDIDs only (`00008110-…`), not
CoreDevice UUIDs from `devicectl list devices`.

## Step 5 — Run ship-preflight

```sh
make ship-preflight
# Should print "READY". Any "BLOCKED:" entries list what still needs fixing.
```

## Step 6 — Plugin symlink (Claude Code, 🎯T64.3)

```sh
make ship-init
```

Idempotent. Creates `~/.claude/plugins/ge` → `ge/` symlink so `/ge:ship`,
`/ge:release-notes`, `/ge:ship-status`, `/ge:onboard` are available in
Claude Code sessions.

## Adding a new app identifier

When you add a new game (e.g. `com.squz.mynewgame`):

```sh
spyder fastlane --studio squz --confirm -- rotate_certs app_identifier:com.squz.mynewgame
```

This mints a new Distribution cert + App Store provisioning profile in the
`squz/certs` repo. Both engineers' laptops pick it up on the next
`spyder fastlane --studio squz -- sync_certs` call.

## Cert rotation

When a Distribution cert expires or is revoked:

```sh
spyder fastlane --studio squz --confirm -- match nuke distribution   # revoke + delete from ASC + repo
spyder fastlane --studio squz --confirm -- rotate_certs app_identifier:com.squz.<game> type:appstore
```

Note: `match nuke` is irreversible. Re-running `rotate_certs` after a
nuke generates new certs that need to land in any games actively shipping
with the old certs — coordinate before nuking.

---

## GitHub Actions setup (🎯T64.6)

The release workflow template at `.github/workflows/release.yml` (in ge) is
copied into each game repo via `make ge/ci-init`. The workflow is tag-triggered:

| Tag pattern | Behaviour |
|---|---|
| `v*` | Android → Play Production; iOS → (laptop, see below) |
| `v*-beta.*` | Android → Play Internal + Closed Beta |

### Runner credentials

The disabled iOS job is not ready to enable on a stock hosted runner. Provision
signed spyder and the runner's studio envelope through an approved setup first;
then use the same missing-secret checks and Make targets as a laptop. Do not
restore parent-shell ASC or match exports. Runner provisioning is a separate
integration task, not implemented by this template.

The existing Android CI path is a separate migration surface. Android signing
is documented in [`android-release.md`](android-release.md), and Play Console
setup in [`play-console.md`](play-console.md). Consumer `testflight` / `playstore`
targets and spyder's custom-lane policy need separate acceptance; this ge change
does not certify those paths.

### Required variables

| Variable | Description |
|---|---|
| `APP_PACKAGE_NAME` | Android package name, e.g. `com.squz.multimaze2` |
| `SHIP_SCHEME` | Xcode scheme name, e.g. `MultiMaze2` |

Set these at the org level for defaults, override at the repo level for per-game values.

### iOS: why laptop-only for now

macOS GitHub Actions runners cost ~10× more than Linux runners. The iOS
workflow job remains disabled. Enabling it requires runner provisioning above,
validation of the consumer targets, and owner approval for publication. Removing
`if: false` alone is insufficient.

### One-time: copy the workflow into a game repo

```sh
cd ~/work/github.com/squz/my-game
make ge/ci-init
# Copies .github/workflows/release.yml from ge into the game repo.
# Idempotent — safe to re-run after ge submodule updates.
```

## Ship checks and remaining acceptance

Each ship lane checks `match` and `pilot` secrets before code generation,
manifests, tags, pushes, worktrees or builds; release also checks `deliver`.
Alpha with `SHIP_DRY_RUN=1` only needs `match` and builds without uploading.
`make ship-preflight LANE=release VERSION=1.2.3` additionally checks repository
cleanliness, app configuration, readonly `sync_certs`, and an optional Xcode pin.
Release requires `CONFIRM=1`; ge forwards `--confirm` to spyder for submission.

Spyder must correctly classify `submit_release` and `upload_testflight external:true`
before full T183 acceptance. That policy change is owned by spyder. The hermetic
ge oracle (`python3 tools/ship/test_ship.py`) tests invocation and refusal behavior;
it does not access real secrets, sign, publish, or establish store acceptance.
