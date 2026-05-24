---
name: onboard
description: Walk a new engineer through setting up a ge-consumer project from scratch to shippable state.
user-invocable: true
---

# /ge:onboard

Complete onboarding guide for a new engineer working on a ge-based game. Run
this on a fresh machine. Follow each step in order; each step verifies its
own outcome before continuing.

## Prerequisites assumed

- macOS (Apple Silicon preferred; Intel works)
- Xcode installed via Mac App Store (not just Command Line Tools)
- Homebrew installed (`/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"`)
- Git configured (`git config --global user.name` and `user.email` set)
- GitHub SSH key or token access to `squz` org repos

---

## Step 1: Clone repos

Clone ge and the consuming game repo:

```bash
# Replace <game> with the actual repo name (e.g. multimaze2, yourworld2)
git clone git@github.com:squz/ge.git ~/work/github.com/squz/ge
git clone git@github.com:squz/<game>.git ~/work/github.com/squz/<game>
cd ~/work/github.com/squz/<game>
git submodule update --init --recursive
```

Verify: `ls ge/Module.mk` prints the file path.

## Step 2: Install project hooks

```bash
cd ~/work/github.com/squz/<game>
git config core.hooksPath scripts/hooks
```

(If `scripts/hooks/` does not exist in the game repo, skip this step.)

## Step 3: Install build prerequisites

```bash
brew install cmake ninja pkg-config git-lfs node
git lfs install
```

Verify: `cmake --version` and `node --version` both print version numbers.

## Step 4: Install Fastlane

```bash
gem install bundler
cd ~/work/github.com/squz/<game>
bundle install
```

Verify: `bundle exec fastlane --version` prints a version number.

## Step 5: Generate App Store Connect API key

You need a `.p8` API key to ship without interactive Apple ID login.

1. Go to https://appstoreconnect.apple.com/access/integrations/api
2. Click "Generate API Key". Role: **App Manager** minimum.
3. Download the `.p8` file. Save it to `~/.fastlane/asc_key.p8`.
4. Note the **Key ID** and **Issuer ID** from the same page.

Set environment variables (add to `~/.zshenv` or `~/.zprofile` for persistence):

```bash
export ASC_KEY_ID="<Key ID>"
export ASC_ISSUER_ID="<Issuer ID>"
export ASC_KEY_FILEPATH=~/.fastlane/asc_key.p8
```

Verify: `echo $ASC_KEY_ID` prints your key ID (not empty).

## Step 6: Set up match passphrase

ge-consumer projects use `fastlane match` for certificate/profile management.
The match repo is encrypted; you need the passphrase.

Get the passphrase from 1Password:

```bash
op read "op://squz/fastlane-match-passphrase/password"
```

Set it as an environment variable (add to `~/.zshenv`):

```bash
export MATCH_PASSWORD="<passphrase from 1Password>"
```

Verify: `echo $MATCH_PASSWORD` prints the passphrase (not empty).

## Step 7: Install the ge Claude Code plugin

This wires up `/ge:ship`, `/ge:release-notes`, `/ge:ship-status`, and
`/ge:onboard` in Claude Code.

```bash
cd ~/work/github.com/squz/<game>
make ship-init
```

The `ship-init` target runs:
```bash
mkdir -p ~/.claude/plugins
ln -s $(pwd)/ge ~/.claude/plugins/ge
```

Verify: `ls ~/.claude/plugins/ge` prints the ge directory contents.

## Step 8: Verify match certificates (read-only)

```bash
cd ~/work/github.com/squz/<game>
bundle exec fastlane match appstore --readonly
```

This should download and install the App Store distribution certificate and
provisioning profile without modifying the match repo.

If this fails:
- **"passphrase incorrect"** — re-check Step 6.
- **"certificate not found"** — you may need `bundle exec fastlane match appstore`
  (without `--readonly`) to generate initial certs. Only an existing team member
  with write access to the match repo can do this.
- **"repo access denied"** — check SSH key access to the match Git repo (ask
  the team for the repo URL if you don't have it).

## Step 9: Build the project

```bash
cd ~/work/github.com/squz/<game>
make
```

This compiles the game server, libge.a, and all dependencies. First build takes
5–15 minutes (bgfx compiles from source).

Verify: `ls bin/` shows the game binary.

## Step 10: Run an alpha ship (optional smoke test)

Once everything above is green, run a dry-run ship to verify the pipeline
end-to-end:

```bash
make ship-preflight
```

If preflight passes, the machine is ready. You can then run `/ge:ship` in
Claude Code and choose "alpha" to push a TestFlight build.

---

## Troubleshooting

**`make ship-preflight` says "No rule to make target"**
The game repo hasn't wired T64.2 make rules yet. Speak to the lead engineer.

**`bundle exec fastlane match appstore --readonly` hangs**
fastlane is waiting for interactive Apple ID login. Ensure `ASC_KEY_ID`,
`ASC_ISSUER_ID`, and `ASC_KEY_FILEPATH` are set and the `.p8` file exists.

**`git submodule update --init --recursive` fails**
Ensure you have SSH access to `squz/ge` (the engine is a private submodule).

**Plugin not appearing in Claude Code**
Restart Claude Code after running `make ship-init`. The plugin is loaded at
startup — changes to `~/.claude/plugins/` require a restart to take effect.
