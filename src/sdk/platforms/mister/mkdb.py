#!/usr/bin/env python3
# ------------------------------------------------------------------------------
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileType: SOURCE
# SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
# ------------------------------------------------------------------------------
"""
mkdb.py — MiSTer Downloader custom-database generator for openfpgaOS games.

Emits the ONE artifact the real MiSTer Downloader
(MiSTer-devel/Downloader_MiSTer) consumes: a ``<game>.json.zip`` — a *ZIP
archive* holding exactly one ``<game>.json`` in the documented schema
(https://github.com/MiSTer-devel/Downloader_MiSTer/blob/main/docs/custom-databases.md).
Despite the ``.zip`` name it is a standard ZIP container, NOT gzip: the
Downloader loads it with ``zipfile.ZipFile`` and requires exactly one member.

Pipeline this fits into:

    repo shippable files (staging dir)  ─┐
    external_files.csv (freeware wads) ──┼─► mkdb.py ─► <game>.json.zip
    base URL / db_id / timestamp ────────┘                 +  downloader.ini snippet

The user registers the DB once by adding the snippet to
``/media/fat/downloader.ini`` (``[<db_id>]`` + ``db_url = <url to the
.json.zip>``); Downloader then syncs the listed files onto the SD card at the
exact paths this DB names.  ``external_files.csv`` is a *build input* to THIS
generator — it is never read on the device.

What the generator does
-----------------------
* Walks a staging directory of shippable files and maps each to its on-SD
  path (``<sd_prefix>/<relpath>``); computes each file's real MD5 + size.
* Local files inherit their download URL from ``base_files_url + <path>`` (the
  Downloader's documented rule for a file with no explicit ``url``), so the
  maintainer hosts the staged tree at that base preserving paths.  (``--url-mode
  flat`` instead emits an explicit per-file ``url = <base>/<basename>`` for a
  flat GitHub-release asset host.)
* Folds ``external_files.csv`` freeware entries in verbatim: each takes its own
  absolute ``url`` / ``size`` / ``md5`` and filter ``tags`` from the CSV.  Rows
  with missing / ``TODO_`` fields (or an unusable URL/size/md5) are skipped with
  a warning — never guessed, never fatal.
* ``boot.vhd`` and anything matched by ``--install-once`` are marked
  ``overwrite:false`` so a later Downloader run never clobbers a user's
  injected wads / in-image saves.

Usage
-----
    mkdb.py --staging build/mister/doom --sd-prefix games/OpenfpgaOS \\
            --base-url https://host/dist/ --db-id owner/openfpgaos-doom \\
            --external dist/doom/mister/external_files.csv \\
            --db-url https://host/dist/doom.json.zip \\
            --output releases/mister/doom.json.zip \\
            --ini-out releases/mister/doom.downloader.ini
"""

import argparse
import csv
import hashlib
import json
import os
import re
import sys
import time
import zipfile
from urllib.parse import urlparse

CHUNK = 1 << 20
_MD5_RE = re.compile(r"^[0-9a-fA-F]{32}$")
# Downloader rejects these as the FIRST path segment (db_entity.invalid_root_folders).
INVALID_ROOT_FOLDERS = ("linux", "screenshots", "savestates", "downloader")
# Default excludes: docs/logs/junk that get staged but must not ship via the DB.
DEFAULT_EXCLUDES = ("*.log", "INSTALL.txt", "._*", ".DS_Store", "Thumbs.db")


def warn(msg):
    print("  [!] mkdb: %s" % msg, file=sys.stderr)


def md5_of(path):
    h = hashlib.md5()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(CHUNK), b""):
            h.update(chunk)
    return h.hexdigest()


def sd_join(prefix, rel):
    """POSIX-join an sd_prefix with a relative path, tolerating empty prefix."""
    rel = rel.replace(os.sep, "/").strip("/")
    prefix = (prefix or "").replace(os.sep, "/").strip("/")
    return (prefix + "/" + rel).strip("/") if prefix else rel


def match_any(name, patterns):
    from fnmatch import fnmatch
    base = os.path.basename(name)
    return any(fnmatch(base, p) or fnmatch(name, p) for p in patterns)


def ancestor_folders(sd_path):
    """All parent folder paths of an sd file path, top-down, no trailing slash."""
    parts = sd_path.split("/")[:-1]
    out = []
    for i in range(1, len(parts) + 1):
        out.append("/".join(parts[:i]))
    return out


def url_is_hostable(url):
    try:
        p = urlparse(url)
        return p.scheme in ("http", "https") and bool(p.hostname) and p.hostname.lower() != "localhost"
    except Exception:
        return False


def is_todo(val):
    return (val is None) or (val == "") or val.strip().upper().startswith("TODO")


def scan_staging(staging, sd_prefix, excludes):
    """Return (files, folders): files = {sd_path: abspath}, folders = set(sd_path)."""
    files = {}
    folders = set()
    staging = os.path.abspath(staging)
    for root, dirs, names in os.walk(staging):
        rel_dir = os.path.relpath(root, staging)
        # Record every directory (including empty ones, e.g. wads/) as a folder.
        if rel_dir != ".":
            fdir = sd_join(sd_prefix, rel_dir)
            if not match_any(fdir, excludes):
                folders.add(fdir)
        for n in sorted(names):
            rel = n if rel_dir == "." else os.path.join(rel_dir, n)
            if match_any(rel, excludes):
                continue
            sd = sd_join(sd_prefix, rel)
            files[sd] = os.path.join(root, n)
    return files, folders


def parse_external(csv_path):
    """Parse external_files.csv → list of {sd_path,url,size,md5,tags}. Skips TODO/invalid rows with a warning."""
    rows = []
    if not csv_path or not os.path.isfile(csv_path):
        if csv_path:
            warn("external CSV not found: %s (no freeware entries added)" % csv_path)
        return rows
    with open(csv_path, newline="") as f:
        reader = csv.reader(f)
        for lineno, raw in enumerate(reader, 1):
            if not raw:
                continue
            first = raw[0].strip()
            if not first or first.startswith("#"):
                continue
            if first.lower() == "sd_path":  # header
                continue
            cols = [c.strip() for c in raw]
            while len(cols) < 5:
                cols.append("")
            sd_path, url, size, md5, tags = cols[0], cols[1], cols[2], cols[3], cols[4]
            if is_todo(sd_path):
                warn("row %d: missing sd_path — skipped" % lineno)
                continue
            missing = [k for k, v in (("url", url), ("size", size), ("md5", md5)) if is_todo(v)]
            if missing:
                warn("row %d (%s): unresolved %s — skipped (maintainer must pin it)"
                     % (lineno, sd_path, "/".join(missing)))
                continue
            if not url_is_hostable(url):
                warn("row %d (%s): url is not a valid http(s) source — skipped" % (lineno, sd_path))
                continue
            try:
                size_i = int(size)
            except ValueError:
                warn("row %d (%s): size %r is not an integer — skipped" % (lineno, sd_path, size))
                continue
            if not _MD5_RE.match(md5):
                warn("row %d (%s): md5 %r is not 32 hex chars — skipped" % (lineno, sd_path, md5))
                continue
            taglist = [t for t in re.split(r"[\s,;]+", tags) if t]
            rows.append({"sd_path": sd_path.replace("\\", "/").strip("/"),
                         "url": url, "size": size_i, "md5": md5.lower(), "tags": taglist})
    return rows


def validate_root(sd_path, kind):
    top = sd_path.split("/")[0].lower()
    if top in INVALID_ROOT_FOLDERS:
        raise SystemExit("mkdb: %s '%s' has a reserved top folder '%s' — Downloader would reject it."
                         % (kind, sd_path, top))


def build_db(args):
    excludes = list(DEFAULT_EXCLUDES) + list(args.exclude or [])
    files_local, folders = scan_staging(args.staging, args.sd_prefix, excludes)
    externals = parse_external(args.external)

    base_url = args.base_url.rstrip("/") + "/" if args.base_url else ""

    files = {}
    # ── local staged files ──────────────────────────────────────────────
    for sd_path in sorted(files_local):
        validate_root(sd_path, "file")
        abspath = files_local[sd_path]
        entry = {"hash": md5_of(abspath), "size": os.path.getsize(abspath)}
        if args.url_mode == "flat":
            if not base_url:
                raise SystemExit("mkdb: --url-mode flat needs --base-url")
            entry["url"] = base_url + os.path.basename(sd_path)
        # base mode: no 'url' → Downloader uses base_files_url + sd_path
        if match_any(sd_path, args.install_once) or os.path.basename(sd_path) == "boot.vhd":
            entry["overwrite"] = False
        if match_any(sd_path, args.reboot or []):
            entry["reboot"] = True
        files[sd_path] = entry

    if args.url_mode == "base" and files and not base_url:
        raise SystemExit("mkdb: local files present but no --base-url (needed for base_files_url).")

    # ── external freeware entries (explicit url/size/md5 from the CSV) ───
    for row in externals:
        sd_path = row["sd_path"]
        validate_root(sd_path, "external file")
        entry = {"hash": row["md5"], "size": row["size"], "url": row["url"]}
        if row["tags"]:
            entry["tags"] = row["tags"]
        if match_any(sd_path, args.install_once):
            entry["overwrite"] = False
        files[sd_path] = entry
        for fdir in ancestor_folders(sd_path):
            folders.add(fdir)

    # Fold in every ancestor folder of every file so the tree is fully declared.
    for sd_path in list(files):
        for fdir in ancestor_folders(sd_path):
            folders.add(fdir)

    db = {
        "v": 1,
        "db_id": args.db_id,
        "timestamp": int(args.timestamp),
    }
    if base_url:
        db["base_files_url"] = base_url
    db["files"] = {k: files[k] for k in sorted(files)}
    db["folders"] = {k: {} for k in sorted(folders)}
    return db, len(files_local), len(externals)


def write_zip(db, out_path):
    os.makedirs(os.path.dirname(os.path.abspath(out_path)) or ".", exist_ok=True)
    inner = os.path.basename(out_path)
    if inner.endswith(".zip"):
        inner = inner[:-4]          # doom.json.zip -> doom.json
    if not inner.endswith(".json"):
        inner += ".json"
    payload = json.dumps(db, separators=(",", ":"), ensure_ascii=False).encode("utf-8")
    # Fixed member metadata → byte-reproducible archive.
    zi = zipfile.ZipInfo(filename=inner, date_time=(1980, 1, 1, 0, 0, 0))
    zi.compress_type = zipfile.ZIP_DEFLATED
    zi.external_attr = 0o644 << 16
    with zipfile.ZipFile(out_path, "w") as zf:
        zf.writestr(zi, payload)
    return inner, len(payload)


def emit_ini(db_id, db_url, ini_out):
    url = db_url if db_url else "https://<PUBLISH-HOST>/<path>/%s.json.zip" % db_id.split("/")[-1]
    snippet = "[%s]\ndb_url = %s\n" % (db_id, url)
    if not db_url:
        snippet = "# TODO: set db_url to where you publish the .json.zip\n" + snippet
    if ini_out:
        os.makedirs(os.path.dirname(os.path.abspath(ini_out)) or ".", exist_ok=True)
        with open(ini_out, "w") as f:
            f.write(snippet)
    return snippet


def main(argv=None):
    ap = argparse.ArgumentParser(description="Generate a MiSTer Downloader custom database (.json.zip).")
    ap.add_argument("--staging", required=True, help="dir of shippable files (its tree maps under --sd-prefix)")
    ap.add_argument("--sd-prefix", default="", help="on-SD path prefix relative to /media/fat (e.g. games/OpenfpgaOS)")
    ap.add_argument("--base-url", default="", help="base_files_url: where the staged tree is hosted")
    ap.add_argument("--db-id", required=True, help="db_id == the downloader.ini [section] (owner/name); never change once published")
    ap.add_argument("--external", default="", help="external_files.csv of freeware entries (sd_path,url,size,md5,tags)")
    ap.add_argument("--timestamp", default=str(int(time.time())), help="unix timestamp for the DB (default: now)")
    ap.add_argument("--output", required=True, help="output <game>.json.zip path")
    ap.add_argument("--ini-out", default="", help="also write the downloader.ini snippet here")
    ap.add_argument("--db-url", default="", help="URL where the .json.zip itself will be hosted (for the ini snippet)")
    ap.add_argument("--url-mode", choices=("base", "flat"), default="base",
                    help="base: local files use base_files_url+path (host mirrors the tree); "
                         "flat: explicit per-file url=<base>/<basename> (flat release assets)")
    ap.add_argument("--install-once", action="append", default=[],
                    help="glob of sd paths to mark overwrite:false (repeatable); boot.vhd is always install-once")
    ap.add_argument("--reboot", action="append", default=[],
                    help="glob of sd paths to mark reboot:true (repeatable)")
    ap.add_argument("--exclude", action="append", default=[],
                    help="extra glob(s) to exclude from the staging walk (repeatable)")
    args = ap.parse_args(argv)

    db, n_local, n_ext = build_db(args)
    inner, jlen = write_zip(db, args.output)
    snippet = emit_ini(args.db_id, args.db_url, args.ini_out)

    print("mkdb: %s  (db_id=%s)" % (args.output, args.db_id))
    print("  files: %d local + %d external = %d   folders: %d   json: %d B"
          % (n_local, n_ext, len(db["files"]), len(db["folders"]), jlen))
    print("  ---- downloader.ini snippet ----")
    for line in snippet.rstrip("\n").splitlines():
        print("  " + line)
    return 0


if __name__ == "__main__":
    sys.exit(main())
