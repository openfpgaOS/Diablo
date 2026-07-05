#!/bin/bash
#------------------------------------------------------------------------------
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileType: SOURCE
# SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
#------------------------------------------------------------------------------

#
# openfpgaOS SDK Setup
#
# Detects your OS, installs the RISC-V toolchain, and validates the
# build environment. Run once after cloning.
#
# Usage:
#   ./scripts/setup.sh              Interactive (prompts before installing)
#   ./scripts/setup.sh --yes        Auto-confirm installs
#

set -e

GREEN='\033[92m'
RED='\033[91m'
YELLOW='\033[93m'
CYAN='\033[96m'
RESET='\033[0m'

ok()   { echo -e "  ${GREEN}+${RESET} $1"; }
fail() { echo -e "  ${RED}x${RESET} $1"; }
warn() { echo -e "  ${YELLOW}!${RESET} $1"; }

AUTO_YES=0
[[ "$1" == "--yes" || "$1" == "-y" ]] && AUTO_YES=1

echo -e "${CYAN}openfpgaOS SDK Setup${RESET}"
echo

# ── Detect OS / distro ───────────────────────────────────────────────
OS="unknown"
DISTRO=""

case "$(uname -s)" in
    Linux)
        OS="linux"
        if [[ -f /etc/os-release ]]; then
            . /etc/os-release
            case "$ID" in
                arch|endeavouros|manjaro|garuda|cachyos)  DISTRO="arch" ;;
                ubuntu|pop|linuxmint|elementary|zorin)     DISTRO="debian" ;;
                debian)                                    DISTRO="debian" ;;
                fedora|nobara)                             DISTRO="fedora" ;;
                opensuse*|sles)                            DISTRO="suse" ;;
                nixos)                                     DISTRO="nix" ;;
                void)                                      DISTRO="void" ;;
                alpine)                                    DISTRO="alpine" ;;
                *)                                         DISTRO="$ID" ;;
            esac
        fi
        # WSL detection
        if grep -qi microsoft /proc/version 2>/dev/null; then
            warn "Running under WSL"
        fi
        ;;
    Darwin)
        OS="macos"
        DISTRO="macos"
        ;;
    MINGW*|MSYS*|CYGWIN*)
        OS="windows"
        DISTRO="msys2"
        ;;
esac

ok "OS: $OS ($DISTRO)"

# ── Check for RISC-V toolchain ──────────────────────────────────────
find_toolchain() {
    if command -v riscv64-unknown-elf-gcc &>/dev/null; then
        echo "riscv64-unknown-elf-"
    elif command -v riscv64-elf-gcc &>/dev/null; then
        echo "riscv64-elf-"
    elif command -v riscv-none-elf-gcc &>/dev/null; then
        echo "riscv-none-elf-"
    else
        echo ""
    fi
}

CROSS=$(find_toolchain)

if [[ -n "$CROSS" ]]; then
    ok "Toolchain: ${CROSS}gcc ($(${CROSS}gcc --version | head -1))"
else
    fail "No RISC-V toolchain found"
    echo

    # Platform-specific install instructions
    INSTALL_CMD=""
    case "$DISTRO" in
        arch)
            echo "  Install with:"
            echo "    sudo pacman -S riscv64-elf-gcc riscv64-elf-newlib"
            INSTALL_CMD="sudo pacman -S --noconfirm riscv64-elf-gcc riscv64-elf-newlib"
            ;;
        debian)
            echo "  Install with:"
            echo "    sudo apt update && sudo apt install gcc-riscv64-unknown-elf"
            INSTALL_CMD="sudo apt update && sudo apt install -y gcc-riscv64-unknown-elf"
            ;;
        fedora)
            echo "  Install with:"
            echo "    sudo dnf install gcc-riscv64-linux-gnu"
            echo "  Or use the xPack toolchain (recommended for baremetal):"
            echo "    https://github.com/xpack-dev-tools/riscv-none-elf-gcc-xpack/releases"
            INSTALL_CMD="sudo dnf install -y gcc-riscv64-linux-gnu"
            ;;
        suse)
            echo "  Install with:"
            echo "    sudo zypper install cross-riscv64-gcc14"
            INSTALL_CMD="sudo zypper install -y cross-riscv64-gcc14"
            ;;
        nix)
            echo "  Add to your environment:"
            echo "    nix-shell -p pkgsCross.riscv64.buildPackages.gcc"
            echo "  Or in flake.nix / shell.nix:"
            echo "    pkgsCross.riscv64.buildPackages.gcc"
            ;;
        void)
            echo "  Install with:"
            echo "    sudo xbps-install cross-riscv64-gcc"
            INSTALL_CMD="sudo xbps-install -y cross-riscv64-gcc"
            ;;
        macos)
            echo "  Install with Homebrew:"
            echo "    brew install riscv64-elf-gcc"
            INSTALL_CMD="brew install riscv64-elf-gcc"
            ;;
        msys2)
            echo "  In MSYS2 UCRT64 terminal:"
            echo "    pacman -S mingw-w64-ucrt-x86_64-riscv64-unknown-elf-gcc"
            INSTALL_CMD="pacman -S --noconfirm mingw-w64-ucrt-x86_64-riscv64-unknown-elf-gcc"
            ;;
        *)
            echo "  Download the xPack RISC-V toolchain:"
            echo "    https://github.com/xpack-dev-tools/riscv-none-elf-gcc-xpack/releases"
            echo "  Extract and add to PATH."
            ;;
    esac

    # Offer to install
    if [[ -n "$INSTALL_CMD" ]]; then
        echo
        if [[ $AUTO_YES -eq 1 ]]; then
            confirm="y"
        else
            read -rp "  Install now? [Y/n] " confirm
            confirm="${confirm:-y}"
        fi
        if [[ "$confirm" =~ ^[Yy] ]]; then
            echo "  Installing..."
            eval "$INSTALL_CMD"
            CROSS=$(find_toolchain)
            if [[ -n "$CROSS" ]]; then
                ok "Installed: ${CROSS}gcc"
            else
                fail "Installation may have succeeded but toolchain not found in PATH"
                fail "Try opening a new terminal and running this script again"
                exit 1
            fi
        else
            echo "  Skipped. Install manually and re-run this script."
            exit 1
        fi
    else
        exit 1
    fi
fi

# ── Validate rv32imafc support ───────────────────────────────────────
CC="${CROSS}gcc"
if $CC -march=rv32imafc -mabi=ilp32f -x c -c /dev/null -o /dev/null 2>/dev/null; then
    ok "Target: rv32imafc (ilp32f)"
    rm -f /dev/null.o 2>/dev/null  # gcc -c /dev/null creates null.o sometimes
else
    fail "Toolchain does not support rv32imafc / ilp32f"
    echo "  You may need a different build of the toolchain."
    echo "  The xPack toolchain supports all RISC-V targets:"
    echo "    https://github.com/xpack-dev-tools/riscv-none-elf-gcc-xpack/releases"
    exit 1
fi

# ── Check make ───────────────────────────────────────────────────────
if command -v make &>/dev/null; then
    ok "make: $(make --version | head -1)"
else
    fail "make not found"
    case "$DISTRO" in
        arch)   echo "    sudo pacman -S make" ;;
        debian) echo "    sudo apt install make" ;;
        fedora) echo "    sudo dnf install make" ;;
        macos)  echo "    xcode-select --install" ;;
    esac
    exit 1
fi

# ── Check runtime files ─────────────────────────────────────────────
SDK_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
RUNTIME="$SDK_ROOT/runtime/pocket"

echo
MISSING=0
# Bitstream(s) are variant-named (os25.rbf_r / os30.rbf_r); at least one
# must be present.  A core picks its variant by name in core.json.
if compgen -G "$RUNTIME"/*.rbf_r >/dev/null 2>&1; then
    for f in "$RUNTIME"/*.rbf_r; do
        ok "$(basename "$f") ($(wc -c < "$f" | tr -d ' ') bytes)"
    done
else
    fail "no *.rbf_r bitstream in runtime/pocket/"
    MISSING=1
fi
for f in os.bin loader.bin; do
    if [[ -f "$RUNTIME/$f" ]]; then
        ok "$f ($(wc -c < "$RUNTIME/$f" | tr -d ' ') bytes)"
    else
        fail "$f not found in runtime/pocket/"
        MISSING=1
    fi
done

[[ $MISSING -ne 0 ]] && warn "Some runtime files missing — download from releases or build from openfpgaOS"

# ── Optional: SDL2 for PC builds ────────────────────────────────────
echo
if command -v sdl2-config &>/dev/null || pkg-config --exists sdl2 2>/dev/null; then
    ok "SDL2: available (for 'make test' desktop builds)"
else
    warn "SDL2 not found — 'make test' won't work (optional)"
    case "$DISTRO" in
        arch)   echo "    sudo pacman -S sdl2" ;;
        debian) echo "    sudo apt install libsdl2-dev" ;;
        fedora) echo "    sudo dnf install SDL2-devel" ;;
        macos)  echo "    brew install sdl2" ;;
    esac
fi

# ── Done ─────────────────────────────────────────────────────────────
echo
echo -e "${GREEN}Ready!${RESET} Create your first app:"
echo "  make core"
