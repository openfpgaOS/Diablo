#!/usr/bin/env python3
"""Verify a Pocket release ZIP against its staged source files and build inputs.

Writes a JSON validation report and SHA-256 sidecar. A stale SDK link is reported
as a pending release check; --require-current-build makes it a hard failure.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import shlex
import struct
import subprocess
import zipfile

ROOT = Path(__file__).resolve().parents[1]


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def require(condition, message):
    if not condition:
        raise ValueError(message)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--archive", type=Path)
    parser.add_argument("--require-current-build", action="store_true")
    args = parser.parse_args()
    dist = ROOT / "dist/diablo"
    core_files = list((dist / "Cores").glob("*/core.json"))
    require(len(core_files) == 1, "Expected exactly one Diablo core")
    core_file = core_files[0]
    metadata = json.loads(core_file.read_text())["core"]
    version = metadata["metadata"]["version"]
    archive = (args.archive or ROOT / f"releases/pocket/diablo-v{version}.zip").resolve()
    elf = ROOT / "src/diablo/app.elf"
    core_dir = core_file.parent.relative_to(dist).as_posix()
    common = "Assets/diablo/common"
    required = {
        f"{common}/diablo.elf": elf,
        f"{common}/os.bin": ROOT / "runtime/pocket/os.bin",
        f"{common}/bank.ofsf": ROOT / "runtime/bank.ofsf",
        f"{common}/devilutionx.mpq": dist / common / "devilutionx.mpq",
        f"{core_dir}/loader.bin": ROOT / "runtime/pocket/loader.bin",
    }
    for core in metadata["cores"]:
        required[f"{core_dir}/{core['filename']}"] = ROOT / "runtime/pocket" / core["filename"]
    # Verify every distributable template, including both game launchers and
    # the save-slot definitions; purchased game MPQs are deliberately omitted.
    for directory in ("Cores", "Assets", "Platforms"):
        for source in (dist / directory).rglob("*"):
            if not source.is_file():
                continue
            if source.suffix.lower() == ".mpq" and source.name.lower() not in ("devilutionx.mpq", "fonts.mpq"):
                continue
            required[source.relative_to(dist).as_posix()] = source

    manifest = {}
    for line in (ROOT / "runtime/MANIFEST").read_text().splitlines():
        if not line or line.startswith("#"):
            continue
        digest, name = line.split(maxsplit=1)
        manifest[name.removeprefix("./")] = digest
    for source in required.values():
        if source.is_relative_to(ROOT / "runtime"):
            name = source.relative_to(ROOT / "runtime").as_posix()
            require(name in manifest, f"Runtime file missing from MANIFEST: {name}")
            require(hashlib.md5(source.read_bytes()).hexdigest() == manifest[name], f"Runtime MANIFEST mismatch: {name}")

    hashes = {}
    with zipfile.ZipFile(archive) as package:
        require(package.testzip() is None, "ZIP CRC check failed")
        names = package.namelist()
        require(len(names) == len(set(names)), "ZIP has duplicate entries")
        require({name.split('/')[0] for name in names} == {"Cores", "Assets", "Platforms", "INSTALL.txt"}, "Unexpected ZIP root layout")
        for name in names:
            require(not name.startswith('/') and '..' not in Path(name).parts, f"Unsafe ZIP path: {name}")
            if name.lower().endswith('.mpq'):
                require(Path(name).name.lower() in ("devilutionx.mpq", "fonts.mpq"), f"Game MPQ in release: {name}")
        for name, source in required.items():
            data = package.read(name)
            require(data == source.read_bytes(), f"Packaged file differs from source: {name}")
            hashes[name] = sha256(data)
        binary = package.read(f"{common}/diablo.elf")
        require(binary[:6] == b'\x7fELF\x01\x01' and struct.unpack_from('<H', binary, 18)[0] == 243, "Not a little-endian 32-bit RISC-V ELF")
        require(b'[of] perf:' not in binary, "Profiling enabled in release ELF")

    config = (ROOT / ".obj/diablo/build-config").read_text()
    require('-DOF_PERF_TRACE' not in config and '-DOF_DIABLO_GPU' not in config, "Experimental build flags enabled")
    require('-march=rv32imafc ' in config, "Unexpected CPU ISA in build config")
    objects = [Path(token) for token in shlex.split(config)
               if token.endswith('.o') and token.startswith(str(ROOT / '.obj/diablo/'))]
    require(bool(objects), "No object provenance found")
    stale = []
    for obj in objects:
        require(obj.is_file(), f"Missing build object: {obj}")
        require(obj.stat().st_mtime_ns <= elf.stat().st_mtime_ns, f"ELF predates object: {obj}")
        dep = obj.with_suffix('.d')
        line = dep.read_text().replace('\\\n', '').splitlines()[0]
        for token in shlex.split(line.split(':', 1)[1]):
            source = Path(token)
            if not source.is_absolute():
                source = ROOT / 'src/diablo' / source
            if not source.is_relative_to(ROOT):
                continue  # Container toolchain headers are unavailable on the host.
            require(source.is_file(), f"Missing workspace dependency: {source}")
            if source.stat().st_mtime_ns > obj.stat().st_mtime_ns:
                stale.append(str(source.relative_to(ROOT)))
    require(not stale, "Object files predate game inputs: " + ', '.join(sorted(set(stale))))
    sdk_inputs = [ROOT / f'src/sdk/musl/lib/{name}' for name in ('crt1.o', 'crti.o', 'crtn.o', 'libc.a', 'libm.a')]
    newer_sdk = [str(path.relative_to(ROOT)) for path in sdk_inputs if path.stat().st_mtime_ns > elf.stat().st_mtime_ns]
    newer_makefile = (ROOT / 'src/diablo/Makefile').stat().st_mtime_ns > elf.stat().st_mtime_ns
    newer_config = (ROOT / '.obj/diablo/build-config').stat().st_mtime_ns > elf.stat().st_mtime_ns
    current_build = not newer_sdk and not newer_makefile and not newer_config
    report = {
        'version': version,
        'checked_at_utc': datetime.now(timezone.utc).isoformat(),
        'git_head': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip(),
        'archive': archive.name,
        'archive_sha256': sha256(archive.read_bytes()),
        'verified_files_sha256': hashes,
        'elf_build_time_utc': datetime.fromtimestamp(elf.stat().st_mtime, timezone.utc).isoformat(),
        'objects_with_current_workspace_dependencies': len(objects),
        'newer_sdk_link_inputs': newer_sdk,
        'makefile_newer_than_elf': newer_makefile,
        'build_config_newer_than_elf': newer_config,
        'build_config_sha256': sha256(config.encode()),
        'current_build': current_build,
        'hardware_retest_of_packaged_candidate': 'pending',
    }
    archive.with_suffix('.validation.json').write_text(json.dumps(report, indent=2) + '\n')
    archive.with_suffix('.zip.sha256').write_text(f"{report['archive_sha256']}  {archive.name}\n")
    print(f"PASS: {archive.name}: ZIP layout, all {len(hashes)} payload files, runtime manifest, release flags, and {len(objects)} object dependency records")
    if not current_build:
        print("PENDING: rebuild with the current Makefile and SDK libraries before publishing")
        if args.require_current_build:
            return 1
    return 0


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except (ValueError, OSError, KeyError, zipfile.BadZipFile) as error:
        raise SystemExit(f"FAIL: {error}")
