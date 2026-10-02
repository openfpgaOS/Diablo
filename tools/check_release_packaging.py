#!/usr/bin/env python3
"""Exercise Pocket ZIPs and release uploads with an offline GitHub CLI fixture."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parents[1]


def write(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(data)


class Fixture:
    def __init__(self, root, release_script, package_script):
        self.root = root
        self.capture = root / "uploads.json"
        write(root / "bin/gh", """#!/usr/bin/env python3
import json, os, sys
from pathlib import Path
args = sys.argv[1:]
if args[:2] == ['auth', 'status']: sys.exit(0)
if args[:2] == ['release', 'view']: sys.exit(1)
if args[:2] == ['release', 'create']:
    Path(os.environ['UPLOAD_CAPTURE']).write_text(json.dumps(args))
    sys.exit(0)
raise SystemExit('Unexpected GitHub operation: ' + repr(args))
""")
        write(root / "bin/git", """#!/usr/bin/env python3
import sys
args = sys.argv[1:]
if args[:3] == ['rev-parse', '-q', '--verify']: sys.exit(1)
if args == ['rev-parse', 'HEAD']: print('0123456789abcdef' * 2 + '01234567'); sys.exit(0)
if args[:2] == ['tag', '--list'] or args == ['status', '--porcelain']: sys.exit(0)
raise SystemExit('Unexpected Git operation: ' + repr(args))
""")
        for name in ("gh", "git"):
            (root / "bin" / name).chmod(0o755)
        self.env = dict(os.environ, PATH=str(root / "bin") + os.pathsep + os.environ["PATH"],
                        UPLOAD_CAPTURE=str(self.capture), PREV="", PUBLISH="0")
        (root / "scripts").mkdir()
        shutil.copy2(release_script, root / "scripts/release.sh")
        self.package_script = package_script
        for target in ("pocket", "mister"):
            dest = root / f"src/sdk/platforms/{target}"
            dest.mkdir(parents=True)
            shutil.copy2(ROOT / f"src/sdk/platforms/{target}/platform.conf", dest)
        self.metadata = json.dumps({"core": {"metadata": {
            "version": "1.1.24", "description": "Doom", "shortname": "doom"}}})

    def pocket(self, label="doom"):
        bundle = self.root / "build/pocket" / label
        write(bundle / "Cores/ThinkElastic.doom/core.json", self.metadata)
        for name in ("os25.rbf_r", "loader.bin"):
            write(bundle / "Cores/ThinkElastic.doom" / name, name)
        for name in ("doom.elf", "os.bin", "bank.ofsf", "doom.ini"):
            write(bundle / "Assets/doom/common" / name, name)
        write(bundle / "Assets/doom/ThinkElastic.doom/Doom2.json", "base launcher")
        write(bundle / "Assets/doom/ThinkElastic.doom/Mod/Doom2.json", "mod launcher")
        write(bundle / "Platforms/doom.json", "{}")
        for name in ("private.wad", "PRIVATE.WAD", "Mixed.WaD"):
            write(bundle / "Assets/doom/common" / name, "private game data")
        # A leftover Downloader DB must not affect Pocket uploads or preflight.
        write(self.root / "releases/pocket/doom.json.zip", "not a Downloader DB")
        write(self.root / "releases/pocket/doom.downloader.ini", "unused")
        return bundle

    def pack(self, label="doom"):
        subprocess.run(["bash", str(self.package_script), "build/pocket/" + label, label, "releases/pocket"],
                       cwd=self.root, env=self.env, check=True, capture_output=True, text=True)
        name = "openfpgaOS-SDK" if label == "sdk" else label
        return self.root / "releases/pocket" / (name + "-v1.1.24.zip")

    def mister(self):
        bundle = self.root / "build/mister/doom"
        write(self.root / "dist/doom/Cores/ThinkElastic.doom/core.json", self.metadata)
        for name in ("boot.vhd", "doom.elf", "doom.ini"):
            write(bundle / "Doom" / name, name)
        write(self.root / "releases/mister/doom-v1.1.24.zip", "bundle")
        write(self.root / "releases/mister/doom.downloader.ini", "database")
        db = {"base_files_url": "https://example.invalid/releases/", "files": {
            "games/Doom/" + name: {} for name in ("boot.vhd", "doom.elf", "doom.ini")}}
        with zipfile.ZipFile(self.root / "releases/mister/doom.json.zip", "w") as archive:
            archive.writestr("doom.json", json.dumps(db))
        return bundle

    def release(self, target, core="doom"):
        return subprocess.run(["bash", str(self.root / "scripts/release.sh"), core],
                              cwd=self.root, env=dict(self.env, TARGET=target),
                              capture_output=True, text=True)

    def uploads(self):
        args = json.loads(self.capture.read_text())
        assert "--draft" in args
        return [Path(p) for p in args[3:args.index("--target")]]


def pocket_layout(fixture):
    bundle = fixture.pocket()
    write(bundle / "Assets/doom/common/stale.ini", "old entry")
    fixture.pack()
    (bundle / "Assets/doom/common/stale.ini").unlink()
    archive = fixture.pack()
    with zipfile.ZipFile(archive) as z:
        assert z.testzip() is None
        names = z.namelist()
        assert {p.split("/")[0] for p in names} == {"Cores", "Assets", "Platforms", "INSTALL.txt"}
        assert not any(p.lower().endswith(".wad") or p.endswith("stale.ini") for p in names)
        assert z.read("Assets/doom/ThinkElastic.doom/Mod/Doom2.json") == b"mod launcher"
        assert z.read("Assets/doom/common/doom.elf") == b"doom.elf"
    assert (bundle / "Assets/doom/common/private.wad").read_text() == "private game data"


def pocket_upload(fixture):
    fixture.pocket()
    (fixture.root / "releases/pocket/doom.json.zip").unlink()
    (fixture.root / "releases/pocket/doom.downloader.ini").unlink()
    archive = fixture.pack()
    result = fixture.release("pocket")
    assert result.returncode == 0, result.stdout + result.stderr
    assert fixture.uploads() == [archive]


def diablo_game_data(fixture):
    bundle = fixture.pocket("diablo")
    # The fixture's generic core/asset directory names do not affect filtering.
    common = bundle / "Assets/doom/common"
    commercial = ("DIABDAT.MPQ", "hellfire.mpq", "HfMoNk.MpQ", "hfmusic.mpq", "hfvoice.mpq", "private.mpq", "mod[1].mpq")
    for name in commercial:
        write(common / name, "private game data")
    write(common / "devilutionx.mpq", "engine assets")
    write(common / "fonts.mpq", "font assets")
    archive = fixture.pack("diablo")
    with zipfile.ZipFile(archive) as z:
        assert z.testzip() is None
        names = {Path(name).name for name in z.namelist()}
        assert not names.intersection(commercial)
        assert {"devilutionx.mpq", "fonts.mpq"} <= names
        assert "Keep your existing Saves/" in z.read("INSTALL.txt").decode()
    for name in commercial:
        assert (common / name).read_text() == "private game data"


def pocket_stale_db(fixture):
    fixture.pocket()
    archive = fixture.pack()
    result = fixture.release("pocket")
    assert result.returncode == 0, result.stdout + result.stderr
    assert fixture.uploads() == [archive]


def sdk_upload(fixture):
    fixture.pocket("sdk")
    archive = fixture.pack("sdk")
    result = fixture.release("pocket", "sdk")
    assert result.returncode == 0, result.stdout + result.stderr
    assert fixture.uploads() == [archive]
    assert archive.name == "openfpgaOS-SDK-v1.1.24.zip"
    # Other apps may have their own WAD-format resources; preserve that policy.
    with zipfile.ZipFile(archive) as z:
        assert z.read("Assets/doom/common/private.wad") == b"private game data"


def mister_upload(fixture):
    fixture.mister()
    result = fixture.release("mister")
    assert result.returncode == 0, result.stdout + result.stderr
    assert {p.name for p in fixture.uploads()} == {
        "doom-v1.1.24.zip", "boot.vhd", "doom.elf", "doom.ini", "doom.json.zip", "doom.downloader.ini"}


def mister_collision(fixture):
    bundle = fixture.mister()
    write(bundle / "Another/doom.elf", "duplicate filename")
    result = fixture.release("mister")
    assert result.returncode != 0 and "basename collision" in result.stdout
    assert not fixture.capture.exists()


def mister_missing(fixture):
    bundle = fixture.mister()
    (bundle / "Doom/doom.elf").unlink()
    result = fixture.release("mister")
    assert result.returncode != 0 and "NOT attached" in result.stdout
    assert not fixture.capture.exists()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--release-script", type=Path, default=ROOT / "scripts/release.sh")
    parser.add_argument("--package-script", type=Path, default=ROOT / "src/sdk/platforms/pocket/package.sh")
    args = parser.parse_args()
    failures = 0
    for check in (pocket_layout, pocket_upload, diablo_game_data, pocket_stale_db, sdk_upload, mister_upload, mister_collision, mister_missing):
        with tempfile.TemporaryDirectory(prefix="doom-package-") as tmp:
            try:
                check(Fixture(Path(tmp) / "repo with spaces", args.release_script.resolve(), args.package_script.resolve()))
                print("PASS:", check.__name__)
            except (AssertionError, subprocess.CalledProcessError) as error:
                failures += 1
                print("FAIL:", check.__name__, error)
    return int(failures != 0)


if __name__ == "__main__":
    raise SystemExit(main())
