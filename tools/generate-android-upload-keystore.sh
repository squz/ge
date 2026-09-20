#!/usr/bin/env bash
# Mint a Play upload key into the macOS keychain (not the filesystem).
exec "$(cd "$(dirname "$0")" && pwd)/android-upload-keychain.sh" mint "$@"
