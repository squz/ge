#!/usr/bin/env bash
# Shared ship front door. Credentials belong to spyder's child, never this shell.
STUDIO="${STUDIO:-squz}"

ship_validate_lane() {
    case "$1" in
        alpha) ;;
        beta|release)
            if [[ ! "$2" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
                echo "ship: VERSION must be MAJOR.MINOR.PATCH for $1" >&2
                return 1
            fi ;;
        *) echo "ship: unknown lane '$1'" >&2; return 1 ;;
    esac
}

ship_check_secrets() {
    local action
    for action in "$@"; do
        spyder secret missing --studio "${STUDIO}" --for "${action}" || return $?
    done
}

ship_check_lane_secrets() {
    ship_check_secrets match || return $?
    # Alpha dry-run still builds/signs, but never uploads.
    if [ "$1" = alpha ] && [ "${SHIP_DRY_RUN:-}" = 1 ]; then
        return 0
    fi
    ship_check_secrets pilot || return $?
    if [ "$1" = release ]; then
        ship_check_secrets deliver || return $?
    fi
}

ship_fastlane() {
    spyder fastlane --studio "${STUDIO}" -- "$@"
}
