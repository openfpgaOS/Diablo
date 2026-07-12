#!/bin/bash
#------------------------------------------------------------------------------
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileType: SOURCE
# SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
#------------------------------------------------------------------------------
#
# Shared container-runtime abstraction sourced by tools/*-container.sh and the
# image-bake scripts.  It auto-detects whether to drive **Docker** (incl.
# OrbStack / Docker Desktop) or **Apple's `container`**
# (https://github.com/apple/container, macOS/Apple-silicon native), and
# normalizes the handful of CLI differences between them so the build scripts
# stay runtime-agnostic and call `oci_*` helpers instead of `docker ...`.
#
# Override the auto-detected runtime explicitly with:
#     OCI=docker      # force Docker / OrbStack
#     OCI=container   # force Apple container
#
# Auto-detection prefers Docker when its CLI is present (the long-proven path),
# and falls back to Apple `container` — so once OrbStack is uninstalled the
# scripts switch to `container` with zero configuration.
#
# Differences handled when the runtime is Apple `container`:
#   -v SRC:DST:ro   ->  --mount type=virtiofs,source=SRC,target=DST,readonly
#                       (Apple container silently ignores a `:ro` bind suffix and
#                        mounts NOTHING — a read-only mount must use --mount.)
#   --tmpfs P:opts  ->  --tmpfs P
#                       (Apple container rejects docker's `:exec`/`:size=` opts
#                        and then mounts nothing; we keep just the path.)
#   --platform linux/amd64  ->  ... --rosetta
#                       (adds fast x86_64 translation for the amd64 Quartus
#                        images; on Apple silicon a bare amd64 run is slow.)
# Everything else (--rm --name -v -w -e --user -t -i --platform --build-arg -f
# -t) is spelled identically by both CLIs and passes straight through.

# ── Runtime detection ──────────────────────────────────────────────────────
oci_detect() {
    if [ -n "${OCI:-}" ]; then
        command -v "$OCI" >/dev/null 2>&1 || {
            echo "ERROR: OCI=$OCI requested but '$OCI' is not on PATH" >&2; return 1; }
        return 0
    fi
    if command -v docker >/dev/null 2>&1; then
        OCI=docker
    elif command -v container >/dev/null 2>&1; then
        OCI=container
    else
        echo "ERROR: no container runtime found — install Docker/OrbStack, or" >&2
        echo "       Apple 'container' (https://github.com/apple/container)."  >&2
        return 1
    fi
}
oci_detect || exit 1
export OCI

oci_is_apple() { [ "$OCI" = container ]; }

# ── Image existence check ───────────────────────────────────────────────────
# `docker image inspect` and `container image inspect` are spelled the same.
oci_image_exists() { "$OCI" image inspect "$1" >/dev/null 2>&1; }

# ── Image build ─────────────────────────────────────────────────────────────
# Both CLIs accept `build [-t NAME] [-f FILE] [--platform P] [--build-arg K=V] CTX`.
# Docker is invoked through buildx so BuildKit Dockerfile features (syntax=,
# RUN --mount) work; Apple container's builder is BuildKit-based natively.
oci_build() {
    if [ "$OCI" = docker ]; then
        # BuildKit Dockerfiles (# syntax= / RUN --mount) need the buildx plugin.
        # Modern Docker routes all BuildKit builds through it; without it you get
        # a cryptic "unknown flag: --platform".  Fail with an actionable message.
        if ! docker buildx version >/dev/null 2>&1; then
            echo "ERROR: this image needs Docker BuildKit, but the 'docker buildx'" >&2
            echo "       plugin is missing.  Install it, then rerun:" >&2
            echo "         Arch:    sudo pacman -S docker-buildx" >&2
            echo "         Debian:  sudo apt-get install docker-buildx-plugin" >&2
            echo "         manual:  drop the buildx binary at ~/.docker/cli-plugins/docker-buildx (chmod +x)" >&2
            return 1
        fi
        DOCKER_BUILDKIT=1 docker buildx build "$@"
    else
        "$OCI" build "$@"
    fi
}

# ── Force-remove a container (Ctrl+C teardown) ──────────────────────────────
oci_rm_force() {
    if [ "$OCI" = container ]; then
        container delete --force "$1" >/dev/null 2>&1 || true
    else
        docker rm -f "$1" >/dev/null 2>&1 || true
    fi
}

# ── Run a container, translating docker-style flags for Apple container ──────
# Pass the FULL docker-style `run` argument list (flags + image + command).
# For Docker it is forwarded verbatim; for Apple container the `:ro` bind,
# `--tmpfs` options, and amd64 Rosetta differences (see header) are rewritten.
oci_run() {
    if [ "$OCI" != container ]; then
        "$OCI" run "$@"
        return $?
    fi
    local out=()
    while [ $# -gt 0 ]; do
        case "$1" in
            -v|--volume)
                case "$2" in
                    *:ro)
                        local rest="${2%:ro}"
                        out+=(--mount "type=virtiofs,source=${rest%%:*},target=${rest#*:},readonly") ;;
                    *) out+=(-v "$2") ;;
                esac
                shift 2 ;;
            --tmpfs)
                out+=(--tmpfs "${2%%:*}"); shift 2 ;;   # drop docker :exec/:size opts
            --platform)
                out+=(--platform "$2")
                [ "$2" = linux/amd64 ] && out+=(--rosetta)   # fast x86 on Apple silicon
                shift 2 ;;
            *) out+=("$1"); shift ;;
        esac
    done
    container run "${out[@]}"
}
