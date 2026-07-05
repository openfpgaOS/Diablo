#!/usr/bin/env python3
#------------------------------------------------------------------------------
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileType: SOURCE
# SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
#------------------------------------------------------------------------------

"""
slotscan_prep — make the Slot Scan experiment survive data.json reverts.

The SDK core's data.json is git-tracked, so a `git checkout`/reset (or any
clean-tree step) in the build/flash cycle reverts it to the committed 19-slot
layout — dropping the high test slots Slot Scan needs.  The Slot Scan
instance.json is untracked, so it keeps binding ids 20/24/28, and the host
then rejects the core with "instance can't match slot id".

Run this from the SDK root *right before* `make` + `make copy`:

    python3 tools/slotscan_prep.py && (cd src/apps && make && make copy)

It idempotently ensures data.json declares the test slots, the instance binds
them, and the marker files exist — for both dist/sdk and build/sdk.
"""
import json, os, sys

CORE = "Cores/ThinkElastic.openfpgaOS/data.json"
INST = "Assets/openfpgaos/ThinkElastic.openfpgaOS/Slot Scan.json"
COMMON = "Assets/openfpgaos/common"
TEST_IDS = [20, 24, 28]          # regular deferload data slots, <= 32-slot cap
APP = "slotscan"

def ensure_data(path):
    d = json.load(open(path))
    slots = d["data"]["data_slots"]
    have = {s["id"] for s in slots}
    for i in TEST_IDS:
        if i not in have:
            slots.append({"id": i, "name": f"Test {i}", "required": False,
                          "parameters": 0, "extensions": ["bin", "dat"],
                          "deferload": True})
    open(path, "w").write(json.dumps(d, indent=4) + "\n")
    ids = [s["id"] for s in d["data"]["data_slots"]]
    assert len(ids) <= 32, f"{len(ids)} slots exceeds APF 32-slot cap"
    return len(ids), max(ids)

def ensure_instance(path):
    slots = [{"id": 1, "filename": "os.bin"},
             {"id": 2, "filename": f"{APP}.ini"},
             {"id": 3, "filename": f"{APP}.elf"}]
    slots += [{"id": i, "filename": f"s{i}.bin"} for i in TEST_IDS]
    inst = {"instance": {"magic": "APF_VER_1",
                         "data_slots": slots}}
    open(path, "w").write(json.dumps(inst, indent=4) + "\n")

def ensure_markers(common_dir):
    os.makedirs(common_dir, exist_ok=True)
    for i in TEST_IDS:
        open(f"{common_dir}/s{i}.bin", "w").write(f"slot {i} marker\n")

def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    os.chdir(root)
    for tree in ("dist/sdk", "build/sdk"):
        core = f"{tree}/{CORE}"
        if not os.path.exists(core):
            print(f"  skip {tree} (no core data.json)")
            continue
        n, mx = ensure_data(core)
        ensure_instance(f"{tree}/{INST}")
        ensure_markers(f"{tree}/{COMMON}")
        print(f"  {tree}: {n} slots, max id {mx}, test slots {TEST_IDS} present")
    # source-of-truth marker files for the release Makefile (*.bin -> common)
    ensure_markers(f"src/apps/{APP}")
    print("OK — data.json + instance + markers consistent. Now: cd src/apps && make && make copy")

if __name__ == "__main__":
    main()
