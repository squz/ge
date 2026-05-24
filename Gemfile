# Gemfile (🎯T64.1) — pins fastlane for reproducible ship runs across both
# engineers' laptops and (later, 🎯T64.6) the GitHub Actions release path.
#
# Install: `bundle install` (one-time per checkout)
# Run:     `bundle exec fastlane <lane>`

source "https://rubygems.org"

# fastlane 2.234.x corresponds to the version on Marcelo's laptop at the
# time of the T64 substrate's introduction (2026-05). Bump in lockstep
# across all squz repos when needed.
gem "fastlane", "~> 2.234"

# match's encryption layer needs a recent OpenSSL on macOS Tahoe.
gem "openssl", ">= 3.0"
