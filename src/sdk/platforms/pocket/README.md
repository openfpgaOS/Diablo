# Pocket packaging

`package.sh` writes one ZIP containing `Cores/`, `Assets/`, `Platforms/` and
`INSTALL.txt` at the archive root. Extract it directly onto the Pocket SD card
and merge the folders. There is no enclosing game or `build/` directory.

`scripts/release.sh` attaches only that ZIP for APF/Pocket releases. Loose
files and Downloader databases belong to image/MiSTer releases, even if a
stale Downloader database exists in the Pocket release directory.

Archives are built in a temporary directory before replacing the output ZIP,
so removed files cannot linger in a repackaged archive. Relative output paths
are resolved before changing into the bundle directory.

DOOM, Heretic and Hexen packages exclude WAD files from the local build tree.
Diablo packages exclude game MPQs while retaining `devilutionx.mpq` and
`fonts.mpq`. Users supply their game data in `Assets/<game>/common/`.

Run `python3 tools/check_release_packaging.py` from the repository root to
check ZIP layout, upload selection and MiSTer Downloader preflight without
network access.
