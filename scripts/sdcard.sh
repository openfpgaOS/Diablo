#!/bin/bash
#------------------------------------------------------------------------------
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileType: SOURCE
# SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
#------------------------------------------------------------------------------

#
# openfpgaOS SDK — SD Card Detection & Mount Helper
#
# Sourceable helper that finds (and optionally mounts) an Analogue Pocket
# SD card across Linux, macOS, and WSL.
#
# After sourcing, $SDCARD is set to the mount point, or the script exits
# with an error.
#
# Usage (from another script):
#   SDCARD="$1"                # let user override
#   source "$(dirname "$0")/sdcard.sh"
#

# ── Already-mounted scan ────────────────────────────────────────────
# Check common mount locations across OSs.
find_pocket_sd() {
    local candidates=()

    case "$(uname -s)" in
        Darwin)
            candidates=(/Volumes/*)
            ;;
        Linux)
            # WSL: Windows drives are mounted under /mnt/<letter>/
            if grep -qi microsoft /proc/version 2>/dev/null; then
                candidates=(/mnt/[a-z]/*)
            else
                # systemd/udisks (Arch, Fedora), older (Ubuntu/Debian), manual
                candidates=(
                    /run/media/"$USER"/*
                    /media/"$USER"/*
                    /mnt/*
                )
            fi
            ;;
        MINGW*|MSYS*|CYGWIN*)
            # Git Bash / MSYS2 / Cygwin on Windows
            candidates=(/[a-z]/*)
            ;;
    esac

    for mount in "${candidates[@]}"; do
        if [[ -d "$mount/Cores" && -d "$mount/Assets" ]]; then
            echo "$mount"
            return
        fi
    done
}

# ── Auto-mount (Linux udisksctl / macOS diskutil) ────────────────────
# Looks for an unmounted FAT32/exFAT partition, mounts it, then
# re-scans. Only runs when the scan above found nothing.
try_mount_sd() {
    case "$(uname -s)" in
        Linux)
            command -v udisksctl >/dev/null 2>&1 || return 1
            command -v lsblk     >/dev/null 2>&1 || return 1

            # Find unmounted removable FAT/exFAT partitions
            local dev
            dev=$(lsblk -rno NAME,FSTYPE,RM,MOUNTPOINT \
                  | awk '$2 ~ /fat|exfat/ && $3 == "1" && $4 == "" { print "/dev/" $1 }' \
                  | head -1)
            [[ -z "$dev" ]] && return 1

            echo "Mounting $dev ..." >&2
            udisksctl mount -b "$dev" --no-user-interaction >/dev/null 2>&1 || return 1

            # Re-scan after mount
            find_pocket_sd
            ;;
        Darwin)
            command -v diskutil >/dev/null 2>&1 || return 1

            # Find unmounted external FAT/exFAT volumes
            local dev
            dev=$(diskutil list external -plist 2>/dev/null \
                  | plutil -extract AllDisksAndPartitions xml1 -o - - 2>/dev/null \
                  | grep -A1 '<key>DeviceIdentifier</key>' \
                  | grep '<string>' | sed 's/.*<string>//;s/<.*//' \
                  | while read -r d; do
                        info=$(diskutil info "/dev/$d" 2>/dev/null)
                        echo "$info" | grep -q 'File System.*FAT\|File System.*ExFAT' || continue
                        echo "$info" | grep -q 'Mounted.*No' || continue
                        echo "/dev/$d"
                        break
                    done)
            [[ -z "$dev" ]] && return 1

            echo "Mounting $dev ..." >&2
            diskutil mount "$dev" >/dev/null 2>&1 || return 1

            # Re-scan after mount
            find_pocket_sd
            ;;
        *)
            return 1
            ;;
    esac
}

# ── Main: resolve $SDCARD ───────────────────────────────────────────
if [[ -z "$SDCARD" ]]; then
    SDCARD="$(find_pocket_sd)"
    if [[ -z "$SDCARD" ]]; then
        SDCARD="$(try_mount_sd)"
    fi
    if [[ -z "$SDCARD" ]]; then
        echo "Error: No Analogue Pocket SD card found."
        echo "       Insert the card or pass the mount path as an argument."
        exit 1
    fi
fi
