#!/usr/bin/env python3
"""Run asset-free host regressions against the production Diablo loaders/shim.

Uses the system C++ compiler; no container, game MPQs, or FPGA is required.
Pass --source-root to compare another checkout against the same regressions.
"""
import argparse
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-root", type=Path, default=ROOT)
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--perf", action="store_true", help="also compile optional Pocket timing instrumentation")
    args = parser.parse_args()
    root = args.source_root.resolve()
    game = root / "src/diablo"
    source = game / "devilutionx/Source"
    compiler = shlex.split(os.environ.get("CXX", "c++"))
    flags = ["-std=gnu++20", "-O1", "-g", "-fno-rtti", "-ffunction-sections", "-fdata-sections",
             "-DOF_PC", "-DOPENFPGAOS", "-DNONET", "-DDVL_NO_FILESYSTEM",
             "-DDEVILUTIONX_DISABLE_RTTI", "-DDEVILUTIONX_DISABLE_EXCEPTIONS",
             "-DDEVILUTIONX_RESAMPLER_SPEEX", "-DDEVILUTIONX_DEFAULT_RESAMPLER=Speex",
             "-DDEVILUTIONX_DISPLAY_TEXTURE_FORMAT=SDL_PIXELFORMAT_RGB888",
             "-DDEVILUTIONX_PALETTE_TRANSPARENCY_BLACK_16_LUT=1"]
    if args.perf:
        flags += ["-DOF_PERF_TRACE"]
    for path in [game, game / "sdl/include", source, game / "3rdparty/fmt/include",
                 game / "3rdparty/simpleini", game / "3rdparty/dr_libs", game / "devilutionx/3rdParty/tl",
                 game / "devilutionx/3rdParty/hoehrmann_utf8", root / "src/sdk/include"]:
        flags += ["-I" + str(path)]
    if args.sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                  "-fno-omit-frame-pointer"]
        # Let the linker discard unrelated game/menu function tables. ASan's
        # global registration otherwise retains the entire engine. Heap and
        # stack accesses in all tested production functions stay instrumented.
        version = subprocess.check_output(compiler + ["--version"], text=True)
        flags += (["-mllvm", "-asan-globals=0"] if "clang" in version
                  else ["--param=asan-globals=0"])
    sources = [game / "sdl/of_sdl2.cpp", source / "monster.cpp", source / "monstdat.cpp",
               source / "engine/load_cl2.cpp", source / "utils/cl2_to_clx.cpp",
               source / "diablo.cpp", source / "player.cpp",
               source / "engine/dx.cpp", source / "engine/palette.cpp", game / "sdl/of_aulib.cpp"]
    with tempfile.TemporaryDirectory(prefix="diablo-regressions-") as tmp:
        objects = []
        for index, path in enumerate(sources):
            obj = str(Path(tmp) / f"{index}.o")
            subprocess.run(compiler + flags + ["-c", str(path), "-o", obj], check=True)
            objects.append(obj)
        failed = False
        for name, linked, extra in [("sdl", objects[:1], []), ("level_memory", objects[:7], []),
                                    ("timing", objects[:1], ["-Wl,--wrap=usleep"]),
                                    ("gamma", [], ["-fno-builtin-powf", "-Wl,--wrap=powf"])]:
            binary = str(Path(tmp) / name)
            test = ROOT / f"src/diablo/tests/{name}_test.cpp"
            subprocess.run(compiler + flags + extra + [str(test)] + linked
                           + ["-Wl,--gc-sections", "-o", binary], check=True)
            result = subprocess.run([binary])
            if result.returncode:
                print(f"FAIL: {name} exited with status {result.returncode}", flush=True)
            failed |= result.returncode != 0
        raise SystemExit(1 if failed else 0)


if __name__ == "__main__":
    main()
