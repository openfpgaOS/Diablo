#!/bin/bash
#------------------------------------------------------------------------------
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileType: SOURCE
# SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
#------------------------------------------------------------------------------
#
# Run an openfpgaOS-SDK app build (or any command) inside the containerized
# RISC-V toolchain.  Mirrors firmware-container.sh but for SDK app builds —
# the toolchain image is shared (riscv64-unknown-elf-gcc + picolibc + musl
# headers + make + python3 + bsdmainutils).
#
# Why this exists: SDK app developers (and downstream cores reusing this
# SDK) shouldn't have to install a RISC-V toolchain by hand — particularly
# painful on macOS where Homebrew's `riscv64-elf-gcc` ships without a libc.
# `bash sdk-container.sh make <targets>` Just Works on any host with a
# container runtime (Docker, or Apple `container` on Apple silicon — see oci.sh).
#
# This script is mirrored to the openfpgaOS-SDK repo (and to any other core
# repo that does `make sdk DEST=<path>`) so SDK consumers get the same
# Docker-only build path without needing the openfpgaCore source.
#
# Usage:
#   bash sdk-container.sh make APP=foo SRCS="main.c"        # one-shot build
#   bash sdk-container.sh -C src/apps/myapp make            # build in subdir
#   bash sdk-container.sh bash                              # interactive shell
#
# Image is auto-built on first use.  Override the toolchain image with
# SDK_IMG=<tag>.  Override the Dockerfile location with SDK_DOCKERFILE=<path>.
set -euo pipefail

# Discover the repo root that contains us.  When this script is shipped INSIDE
# the SDK repo (or a downstream core repo) it'll resolve to that repo's root;
# when invoked from openfpgaCore directly, it resolves to openfpgaCore.  Either
# way, that root is bind-mounted into the container at the same path so
# absolute paths in app Makefiles resolve identically inside.
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMG="${SDK_IMG:-openfpgaos-firmware}"
DOCKERFILE="${SDK_DOCKERFILE:-$REPO/tools/docker/Dockerfile.firmware}"
source "$REPO/tools/oci.sh"   # $OCI + oci_run/oci_build/oci_image_exists/oci_rm_force

# Optional -C <dir> like make: change to <dir> before running the command.
# Resolve symlinks (pwd -P) so the WORKDIR prefix matches REPO — on macOS,
# /tmp is a symlink to /private/tmp, and bind-mounting /private/tmp/... while
# -w /tmp/... gives the container an inconsistent view and `make -f Makefile`
# claims the makefile is missing.
WORKDIR="$(pwd -P)"
if [ "${1:-}" = "-C" ]; then
    WORKDIR="$(cd "$2" && pwd -P)"
    shift 2
fi
[ $# -ge 1 ] || { echo "usage: sdk-container.sh [-C <dir>] <command> [args...]"; exit 2; }

# Build the image on first use (one-time).  Auto-builds match the pattern
# used by firmware-container.sh / vexii-container.sh — fresh checkout +
# Docker is enough.
if ! oci_image_exists "$IMG"; then
    [ -f "$DOCKERFILE" ] || { echo "ERROR: $DOCKERFILE missing — set SDK_DOCKERFILE=<path>"; exit 1; }
    echo "[sdk] building $IMG image with $OCI (one-time, ~2-3 min)..."
    oci_build -t "$IMG" -f "$DOCKERFILE" "$(dirname "$DOCKERFILE")"
fi

# Pseudo-TTY only when our stdout is a terminal, so an interactive build
# keeps gcc's colored output but a redirected/piped run keeps logs clean.
# The ${TTY[@]+"${TTY[@]}"} idiom is required for bash 3.2 (macOS default)
# under set -u — plain "${TTY[@]}" errors on an empty array there.
TTY=()
# Both stdin and stdout must be TTYs — `-i -t` fails ("cannot attach stdin to a
# TTY-enabled container...") when stdin isn't a terminal, e.g. under make.
[ -t 0 ] && [ -t 1 ] && TTY=(-i -t)

# --user keeps outputs owned by the host user.  Repo bind-mounted at the
# SAME path so absolute paths inside the app Makefile resolve identically.
# WORKDIR (cwd inside the container) is whatever the user was in on the
# host (or wherever -C took us) — feels like running make locally.
#
# HOME goes on a dedicated tmpfs at /sdkhome (not /tmp) to avoid shadowing
# repo bind-mounts whose host path happens to live under /tmp.
# Name the container + tear it down from a trap so Ctrl+C (SIGINT) cleanly
# kills the build instead of orphaning it.  When stdin is a TTY we pass -i -t
# (above) and docker forwards signals itself, but for redirected/piped runs
# the trap is what guarantees the container dies.  Run in the background and
# `wait` (interruptible) so the trap fires on INT/TERM/EXIT.
CNAME="ofpgaos-sdk-$$"
trap 'oci_rm_force "$CNAME"' EXIT INT TERM
# Backgrounding below (so the EXIT/INT trap cleans up the container on Ctrl+C)
# detaches stdin to /dev/null, which a -i -t TTY rejects ("cannot attach stdin
# to a TTY-enabled container because stdin is not a terminal") — exactly what
# an interactive `make` hits now that the container is the default build path.
# When a TTY was allocated, hand the controlling terminal (/dev/tty) to the
# backgrounded run; otherwise /dev/null is correct.  Same fix as
# quartus17-container.sh.
STDIN_SRC=/dev/null
[ ${#TTY[@]} -gt 0 ] && STDIN_SRC=/dev/tty
# The shared image sets CPATH=/usr/lib/picolibc/... for the firmware build,
# but SDK apps link musl and must NOT see picolibc (its types collide with
# musl's, and CPATH dirs survive -nostdinc).  Clear CPATH for the app build.
# OF_SDK_IN_CONTAINER=1 tells sdk.mk it is now INSIDE the containerized
# toolchain (its default-path app.elf rule re-execs make through this script
# and must not recurse).  MAKEFLAGS is forwarded so command-line variable
# overrides (e.g. `make build CFLAGS=-DDEBUG`) survive the re-exec.
# A jobserver-style MAKEFLAGS (from an outer `make -jN`) references pipe
# fds / a fifo that don't exist inside the container — the inner make would
# warn and fall back to SERIAL.  Rewrite it to a plain -j<nproc> so inner
# builds keep their parallelism.
MF="${MAKEFLAGS:-}"
if [[ "$MF" == *jobserver* ]]; then
    MF="$(printf '%s' "$MF" | sed -E 's/--jobserver-(auth|fds|style)=[^ ]+//g; s/(^| )-j[0-9]*( |$)/\1\2/g')"
    MF="$MF -j$(nproc)"
fi
oci_run --rm --name "$CNAME" ${TTY[@]+"${TTY[@]}"} \
  --user "$(id -u):$(id -g)" \
  -v "$REPO:$REPO" \
  --tmpfs /sdkhome:exec \
  -e HOME=/sdkhome \
  -e CPATH= \
  -e OF_SDK_IN_CONTAINER=1 \
  -e "MAKEFLAGS=$MF" \
  -w "$WORKDIR" \
  "$IMG" \
  "$@" < "$STDIN_SRC" &
wait $!
