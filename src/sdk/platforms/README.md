# SDK platforms — adding a target

A **platform** is one runtime target you can build apps and cores for
(Analogue Pocket, MiSTer, …). The SDK discovers platforms from the
directories in here, so adding a target is **add a directory** — the
shared `Makefile` and `scripts/` name no target.

Every verb honours `TARGET=<name>` (default `pocket`):

```
make build   CORE=<core> TARGET=<name>     # → src/<core> → image.sh
make build   APP=<app>   TARGET=<name>     # → SDK demo app → image.sh
make copy    CORE=<core> TARGET=<name>     # → copy.sh
make package CORE=<core> TARGET=<name>     # → package.sh → releases/<name>/
make release CORE=<core> TARGET=<name>     # → platform.conf → GitHub release
```

`make help` lists `TARGET=<discovered platforms>` automatically.

## What a platform directory must provide

`src/sdk/platforms/<name>/`:

### `platform.conf`  (sourced by `scripts/release.sh`)
```sh
PLATFORM_PRODUCT="MiSTer"          # shown in the release title
PLATFORM_TAG_SUFFIX="-mister"      # git tag = <core><suffix>-v<ver>  ("" for none)
PLATFORM_BUNDLE_KIND="image"       # apf | image  — shape of build/<name>/<core>/
PLATFORM_COREJSON="dist"           # bundle | dist — where core.json is read from
```

### `image.sh <app> <elf> <sdk_root> <dist_dir|""> [assets...]`
Assemble ONE app's deliverable into `build/<name>/<app>/`.
- Custom cores pass `dist_dir`; SDK demo apps pass `""`.
- If your platform has a shared multi-app demo core (like Pocket), no-op
  when `dist_dir` is empty — those apps bundle into the demo core via
  `make build CORE=sdk`.
- Must be executable (`chmod +x`); the Makefile/template invoke it directly.

### `copy.sh <app> <elf> [host]`
Deploy one app/core to the device. The host (e.g. `MISTER_IP`) also
arrives via the **environment**, so the shared `copy` rule passes no
per-target argument. Support a `core` mode (`copy.sh core <host>`) for
core-only bring-up.

### `package.sh <build_dir> <label> <releases_dir>`
Zip one built bundle into `<releases_dir>/`. Exit 0 (skip) if
`<build_dir>` isn't your bundle kind. Invoked via `bash`, so no exec bit
needed (but harmless).

### Optional helpers
Whatever your platform needs — MiSTer ships `mkimage.{c,sh}` + `fatfs/`
(a FAT32 image builder); Pocket ships `templates/` (APF JSON manifests
for `make core`).

## runtime/<name>/

The OS repo's `make sdk DEST=<this-sdk>` populates `runtime/<name>/` with
your target's runtime artifacts (it runs each OS target's
`sdk-runtime.sh`) — the kernel, plus the core bitstream for targets that
vendor it per app. A target may instead install its core from a separate
release rather than syncing the bitstream here (MiSTer syncs only the
kernel; its game-agnostic core ships via `make package TARGET=mister` and
is installed once to `_Computer/OpenfpgaOS.rbf`). Your platform scripts
read their artifacts from there.
Every target uses `runtime/<name>/` (pocket → `runtime/pocket/`, mister →
`runtime/mister/`); only the target-agnostic `bank.ofsf` lives at
`runtime/` root.

## Checklist

- [ ] `platforms/<name>/{platform.conf, image.sh, copy.sh, package.sh}`
- [ ] `image.sh`, `copy.sh` are `chmod +x`
- [ ] `runtime/<name>/` populated (`make sdk DEST=…` from openfpgaOS)
- [ ] `make build APP=<app> TARGET=<name>` and `make package TARGET=<name>` work
- [ ] (optional) add a one-line description to the root Makefile's help note

See `openfpgaOS/docs/ADDING_A_TARGET.md` for the OS-repo side (the FPGA
target + `sdk-runtime.sh` that fills `runtime/<name>/`).
