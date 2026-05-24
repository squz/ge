# Release setup (🎯T64.1 — studio-wide signing via fastlane match)

This is the **one-time setup** a new engineer or a fresh machine needs before
running any of the `make ship-*` lanes (🎯T64.2). After this, day-to-day
shipping is `bundle exec fastlane match appstore --readonly` (resolved
automatically by the ship lanes), no prompts, no per-game cert dance.

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

## Step 2 — Shell environment

Add to your shell profile (`~/.zshrc` or equivalent):

```sh
export APP_STORE_CONNECT_API_KEY_KEY_ID="<your KEYID from Step 1>"
export APP_STORE_CONNECT_API_KEY_ISSUER_ID="<the issuer UUID>"
export APP_STORE_CONNECT_API_KEY_KEY_PATH="$HOME/.appstoreconnect/private_keys/AuthKey_${APP_STORE_CONNECT_API_KEY_KEY_ID}.p8"

# Match passphrase — paste from 1Password ("squz / match passphrase").
export MATCH_PASSWORD="<passphrase>"

# Per-game: the Xcode scheme name (e.g. "MultiMaze2"). Set in the game's
# .zshrc or in the Makefile as SHIP_SCHEME := MyGame.
export SHIP_SCHEME="<your Xcode scheme>"
```

Reload your shell or `source ~/.zshrc`.

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
bundle exec fastlane sync_certs
# Should resolve and install certs silently. If it prompts for a password,
# double-check MATCH_PASSWORD and the squz/certs SSH key.
```

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
bundle exec fastlane rotate_certs app_identifier:com.squz.mynewgame
```

This mints a new Distribution cert + App Store provisioning profile in the
`squz/certs` repo. Both engineers' laptops pick it up on the next
`sync_certs` / `bundle exec fastlane match appstore --readonly` call.

## Cert rotation

When a Distribution cert expires or is revoked:

```sh
bundle exec fastlane match nuke distribution   # revoke + delete from ASC + repo
bundle exec fastlane rotate_certs app_identifier:com.squz.<game> type:appstore
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

### Required secrets

Configure these at the **organisation level** (squz or minicadesmobile) so
every game repo inherits them without per-repo setup:

| Secret | Description |
|---|---|
| `ASC_API_KEY_ID` | App Store Connect API Key ID (from Step 1 above) |
| `ASC_API_ISSUER_ID` | App Store Connect Issuer ID |
| `ASC_API_KEY` | Contents of the `.p8` key file, base64-encoded: `base64 -i AuthKey_XXXXX.p8` |
| `MATCH_PASSWORD` | Match passphrase (same as your shell env var) |
| `PLAY_SERVICE_ACCOUNT_JSON` | Google Play service account JSON, base64-encoded |

### Required variables

| Variable | Description |
|---|---|
| `APP_PACKAGE_NAME` | Android package name, e.g. `com.squz.multimaze2` |
| `SHIP_SCHEME` | Xcode scheme name, e.g. `MultiMaze2` |

Set these at the org level for defaults, override at the repo level for per-game values.

### iOS: why laptop-only for now

macOS GitHub Actions runners cost ~10× more than Linux runners. The iOS
workflow job is scaffolded (secrets + steps documented) but disabled via
`if: false`. Enabling it requires:

1. Remove the `if: false` line from the `ios:` job in the workflow file.
2. Confirm the org-level secrets listed above are set.
3. Push a tag.

Estimated migration time: ~30 minutes. Do it when macOS runner costs are
acceptable or when the team grows beyond one laptop.

### One-time: copy the workflow into a game repo

```sh
cd ~/work/github.com/squz/my-game
make ge/ci-init
# Copies .github/workflows/release.yml from ge into the game repo.
# Idempotent — safe to re-run after ge submodule updates.
```
