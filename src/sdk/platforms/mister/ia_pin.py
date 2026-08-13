#!/usr/bin/env python3
# ------------------------------------------------------------------------------
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileType: SOURCE
# SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
# ------------------------------------------------------------------------------
"""
ia_pin.py — resolve external_files.csv TODO rows against an archive.org item.

mkdb.py refuses to ship an external entry without a real size+md5, so every
freeware row starts life as TODO_URL/TODO_SIZE/TODO_MD5 and someone has to pin
it by hand.  archive.org publishes both for every file in an item, so this
fetches the item metadata once and fills the rows in.

    ia_pin.py --csv dist/doom/mister/external_files.csv --subdir doom
    ia_pin.py --csv ... --subdir doom --write        # apply (default is dry-run)

IT ONLY FILLS ROWS THAT ALREADY EXIST.  It never adds a file to the CSV.  That
is deliberate: an archive item is a bucket somebody else curated, and for the
openFPGA-Files item most of what is in it is commercial game data that this
project deliberately does NOT redistribute (users supply their own IWADs/ISOs —
see the INSTALL.txt the packager writes).  Deciding a file is freely
distributable is a licensing judgement, so it stays with the maintainer; this
tool only does the mechanical part they would otherwise do by hand.

Match is by the sd_path basename, case-insensitive.  Use --alias LOCAL=REMOTE
when the shipped name differs from the archive's (e.g. rekkrsa.wad=REKKR.WAD).
"""

import argparse
import csv
import io
import json
import os
import sys
import urllib.parse
import urllib.request

IA_METADATA = "https://archive.org/metadata/{item}"
IA_DOWNLOAD = "https://archive.org/download/{item}/{path}"
TODO_FIELDS = ("url", "size", "md5")


def is_todo(val):
    return (val or "").strip().upper().startswith("TODO")


def fetch_item(item, timeout):
    url = IA_METADATA.format(item=item)
    with urllib.request.urlopen(url, timeout=timeout) as r:
        return json.loads(r.read().decode("utf-8"))


def index_files(meta, subdir):
    """Return (by_base, by_relpath).

    by_base maps a lowercased basename to the LIST of matching archive files --
    a list, not a single entry, because an item can hold the same basename in
    two folders with completely different contents.  openFPGA-Files does exactly
    that: quake/pak0.pak is the 18 MB shareware pak while quake/HIPNOTIC/PAK0.PAK
    is a 35 MB commercial mission pack.  Silently taking the first match pinned
    the wrong file (and the wrong licence) with a perfectly valid-looking hash,
    so ambiguity is now reported and the caller must disambiguate with --alias.
    """
    by_base, by_rel = {}, {}
    prefix = (subdir.strip("/") + "/") if subdir else ""
    for f in meta.get("files", []):
        name = f.get("name", "")
        if prefix and not name.startswith(prefix):
            continue
        if not f.get("md5") or not f.get("size"):
            continue
        rec = (name, f["size"], f["md5"])
        rel = name[len(prefix):] if prefix else name
        by_rel[rel.lower()] = rec
        by_base.setdefault(os.path.basename(name).lower(), []).append(rec)
    return by_base, by_rel


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--csv", required=True, help="external_files.csv to pin")
    ap.add_argument("--item", default="openFPGA-Files", help="archive.org item id")
    ap.add_argument("--subdir", default="", help="restrict to this folder in the item")
    ap.add_argument("--alias", action="append", default=[],
                    help="LOCAL=REMOTE name mapping (repeatable)")
    ap.add_argument("--write", action="store_true", help="apply (default: dry-run)")
    ap.add_argument("--timeout", type=int, default=60)
    args = ap.parse_args(argv)

    aliases = {}
    for a in args.alias:
        if "=" not in a:
            sys.exit("ia_pin: --alias needs LOCAL=REMOTE, got %r" % a)
        k, v = a.split("=", 1)
        aliases[k.strip().lower()] = v.strip().lower()

    with open(args.csv, newline="", encoding="utf-8") as fh:
        raw = fh.read()

    # Preserve the leading comment block verbatim -- it documents provenance.
    lines = raw.splitlines(True)
    head = [l for l in lines if l.lstrip().startswith("#")]
    body = "".join(l for l in lines if not l.lstrip().startswith("#"))

    rdr = csv.DictReader(io.StringIO(body))
    rows = list(rdr)
    field_order = rdr.fieldnames
    if not field_order:
        sys.exit("ia_pin: %s has no header row" % args.csv)

    try:
        meta = fetch_item(args.item, args.timeout)
    except Exception as e:
        sys.exit("ia_pin: fetching %s failed: %s" % (args.item, e))
    by_base, by_rel = index_files(meta, args.subdir)
    print("ia_pin: %s/%s -> %d file(s) with size+md5"
          % (args.item, args.subdir or "*", len(by_rel)))

    pinned, skipped, unmatched, ambiguous = 0, 0, [], []
    for row in rows:
        todo = [f for f in TODO_FIELDS if is_todo(row.get(f))]
        if not todo:
            skipped += 1
            continue
        sd_path = row.get("sd_path", "")
        base = os.path.basename(sd_path).lower()
        key = aliases.get(base, base)
        # An alias containing "/" names an exact path inside the item; that is
        # the only way to pick between same-basename files.
        remote = by_rel.get(key)
        if remote is None:
            cands = by_base.get(key, [])
            if len(cands) > 1:
                ambiguous.append((sd_path, [c[0] for c in cands]))
                continue
            remote = cands[0] if cands else None
        if not remote:
            unmatched.append(sd_path)
            continue
        path, size, md5 = remote
        row["url"] = IA_DOWNLOAD.format(item=args.item,
                                        path=urllib.parse.quote(path))
        row["size"] = str(size)
        row["md5"] = md5
        pinned += 1
        print("  pinned %-46s %10s B  %s" % (os.path.basename(path), size, md5))

    for sd in unmatched:
        print("  [!] no archive match, left as TODO: %s" % sd)
    for sd, cands in ambiguous:
        print("  [!] AMBIGUOUS, left as TODO: %s" % sd)
        for c in cands:
            print("        candidate: %s" % c)
        print("        disambiguate with --alias %s=<exact/path/in/item>"
              % os.path.basename(sd))
    print("ia_pin: %d pinned, %d already complete, %d unmatched, %d ambiguous"
          % (pinned, skipped, len(unmatched), len(ambiguous)))
    if ambiguous:
        return 2

    if not args.write:
        print("ia_pin: dry-run (pass --write to apply)")
        return 0
    if not pinned:
        print("ia_pin: nothing to write")
        return 0

    buf = io.StringIO()
    w = csv.DictWriter(buf, fieldnames=field_order, lineterminator="\n")
    w.writeheader()
    for row in rows:
        w.writerow(row)
    with open(args.csv, "w", encoding="utf-8", newline="") as fh:
        fh.write("".join(head))
        fh.write(buf.getvalue())
    print("ia_pin: wrote %s" % args.csv)
    return 0


if __name__ == "__main__":
    sys.exit(main())
