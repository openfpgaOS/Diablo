#!/bin/bash
#------------------------------------------------------------------------------
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileType: SOURCE
# SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
#------------------------------------------------------------------------------

#
# openfpgaOS SDK — Validate a per-instance .ini for the MiSTer two-root
# F-load model.
#
# The firmware (targets/mister/file.c, kernel/main.c) builds two roots from
# the F-loaded os.ini's [os] section:
#   common_root   = "/<GAME>/common"     (READ  — elf, bank.ofsf, wads)
#   instance_root = "/<GAME>/<INSTANCE>" (WRITE — config + saves, flat)
# Both keys must be present and non-empty or the OS silently falls back to
# the legacy single-image layout — a broken per-game package.  This is the
# enforcement point: mkgame.sh / mkmgl.sh refuse to package an ini that
# would not resolve its two roots.
#
# An ini MUST carry, in [os]:  GAME=  INSTANCE=  ELF=
# Given an expected game name it must match GAME (case-insensitive); given
# an expected stem, INSTANCE is checked non-empty (a stem mismatch is a
# warning, not fatal — a stem is just the on-card ini filename).
#
# Two ways to use it:
#   ./validate-ini.sh <instance.ini> [expected_game] [expected_stem]   (exit 0/1)
#   source validate-ini.sh; validate_ini <ini> [game] [stem]           (function)
#
# On success it prints "GAME<TAB>INSTANCE<TAB>ELF" to stdout so callers can
# capture the parsed triple without re-parsing.
#
# Platform: MiSTer (DE10-Nano / SuperStation One)
#

# ── Parse the [os] section (case-insensitive keys, last-wins, '#'/';'
#    comments — the same grammar as the firmware's config.c) ────────────
# Sets the caller-visible globals VI_GAME / VI_INSTANCE / VI_ELF.
_vi_parse() {
    local ini="$1" line t key val section=""
    VI_GAME=""; VI_INSTANCE=""; VI_ELF=""; VI_HAVE_OS=0
    while IFS= read -r line || [[ -n "$line" ]]; do
        line="${line%$'\r'}"
        t="${line#"${line%%[![:space:]]*}"}"; t="${t%"${t##*[![:space:]]}"}"
        [[ -z "$t" || "$t" == \#* || "$t" == \;* ]] && continue
        if [[ "$t" == \[*\] ]]; then
            section="${t:1:${#t}-2}"; section="${section,,}"
            [[ "$section" == "os" ]] && VI_HAVE_OS=1
            continue
        fi
        [[ "$t" == *=* ]] || continue
        key="${t%%=*}"; val="${t#*=}"
        key="${key%"${key##*[![:space:]]}"}"
        val="${val#"${val%%[![:space:]]*}"}"; val="${val%"${val##*[![:space:]]}"}"
        [[ "$section" == "os" ]] || continue
        case "${key,,}" in
            game)     VI_GAME="$val" ;;
            instance) VI_INSTANCE="$val" ;;
            elf)      VI_ELF="$val" ;;
        esac
    done < "$ini"
}

# Diagnostic emitters (stderr; the parsed triple goes to stdout on success).
_vi_err() { echo -e "  \033[91mx\033[0m ${_VI_TAG}: $1" >&2; }
_vi_wrn() { echo -e "  \033[93m!\033[0m ${_VI_TAG}: $1" >&2; }

# validate_ini <instance.ini> [expected_game] [expected_stem]
# Returns 0 on success (and echoes "GAME\tINSTANCE\tELF"), 1 on failure.
validate_ini() {
    local ini="$1" want_game="${2:-}" want_stem="${3:-}"
    _VI_TAG="$(basename "$ini")"

    [[ -f "$ini" ]] || { _vi_err "not found"; return 1; }
    _vi_parse "$ini"

    [[ "$VI_HAVE_OS" == 1 ]] || { _vi_err "no [os] section"; return 1; }
    [[ -n "$VI_GAME" ]]      || { _vi_err "[os] GAME= missing or empty (needed for common_root=/<GAME>/common)"; return 1; }
    [[ -n "$VI_INSTANCE" ]]  || { _vi_err "[os] INSTANCE= missing or empty (needed for instance_root=/<GAME>/<INSTANCE>)"; return 1; }
    [[ -n "$VI_ELF" ]]       || { _vi_err "[os] ELF= missing or empty (no app to launch)"; return 1; }

    if [[ -n "$want_game" && "${VI_GAME,,}" != "${want_game,,}" ]]; then
        _vi_err "GAME=\"$VI_GAME\" does not match expected game \"$want_game\""
        return 1
    fi
    if [[ -n "$want_stem" && "${VI_INSTANCE,,}" != "${want_stem,,}" ]]; then
        _vi_wrn "INSTANCE=\"$VI_INSTANCE\" != ini stem \"$want_stem\" (mgl F-loads by stem; check the pairing)"
    fi

    printf '%s\t%s\t%s\n' "$VI_GAME" "$VI_INSTANCE" "$VI_ELF"
    return 0
}

# ── Standalone entry point (no-op when sourced) ──────────────────────────
if [[ "${BASH_SOURCE[0]}" == "${0}" ]]; then
    if [[ -z "$1" ]]; then
        echo "Usage: $0 <instance.ini> [expected_game] [expected_stem]" >&2
        exit 1
    fi
    validate_ini "$@"
    exit $?
fi
