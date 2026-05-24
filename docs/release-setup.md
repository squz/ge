# Release setup (🎯T64.1 — studio-wide signing via fastlane match)

This is the **one-time setup** a new engineer or a fresh machine needs before
running any of the `make ship-*` lanes (🎯T64.2). After this, day-to-day
shipping is `bundle exec fastlane match appstore --readonly` (resolved
automatically by the ship lanes), no prompts, no per-game cert dance.

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

# Optional: where Ruby + fastlane live. If you used Homebrew's `ruby`,
# these defaults usually work without override.
```

Reload your shell or `source ~/.zshrc`.

## Step 3 — Ruby + fastlane

```sh
cd ~/work/github.com/squz/ge
bundle install        # reads Gemfile, installs fastlane 2.234.x
```

If `bundle` complains about Ruby version, install the recommended one via
Homebrew: `brew install ruby` and add `/opt/homebrew/opt/ruby/bin` to your
`PATH` ahead of the system Ruby.

## Step 4 — Verify

```sh
cd ~/work/github.com/squz/ge
bundle exec fastlane match appstore --readonly
```

Expected: fastlane clones `squz/certs`, decrypts with `MATCH_PASSWORD`,
imports certs into your Keychain. No prompts. Output ends with
`Successfully installed certificate`.

If it asks for a password, your `MATCH_PASSWORD` env var isn't set or is
wrong. If it errors on the git fetch, you don't yet have access to
`squz/certs` — message Marcelo.

## Step 5 — Plugin symlink (Claude Code, 🎯T64.3)

```sh
make ship-init
```

Idempotent. Creates `~/.claude/plugins/ge` → `ge/` symlink so `/ge:ship`
and friends are available in Claude Code sessions.

## Adding a new app identifier (rare — only Marcelo or whoever owns the
new game does this)

```sh
cd ~/work/github.com/squz/ge
bundle exec fastlane rotate_certs app_identifier:com.squz.newgame type:appstore
```

This mints a fresh certificate + provisioning profile for
`com.squz.newgame`, encrypts it, and pushes to `squz/certs`. Other
engineers pick it up on their next `match appstore --readonly`.

## Rotating expired or compromised certs

```sh
cd ~/work/github.com/squz/ge
bundle exec fastlane match nuke distribution   # revoke + delete from ASC + repo
bundle exec fastlane rotate_certs app_identifier:com.squz.<game> type:appstore
```

Note: `match nuke` is irreversible. Re-running `rotate_certs` after a
nuke generates new certs that need to land in any games actively shipping
with the old certs — coordinate before nuking.
