#!/usr/bin/env python3
"""Build the release assets of an Orca Zoukei update (Windows) and print the `gh release create` command.

    python tools/zoukei/make_release.py --tree build/OrcaSlicer --version 2026.10.17 --fetch-previous 3

Produces in --out (default dist/release-<version>):
    manifest-<platform>.json                 per-file SHA-256 + size, the full zip and the deltas
    OrcaZoukei-<version>-<platform>.zip      the whole install tree (also usable as a fresh install)
    delta-<from>-<to>-<platform>.zip         only the files that changed since <from> (one per previous release)
    SHA256SUMS.txt

The script never talks to GitHub except to READ the manifests of earlier releases (--fetch-previous),
and it never creates a release: it prints the command for the owner to run. See docs/zoukei/updates.md.
Only the Python standard library is used.
"""
import argparse
import fnmatch
import hashlib
import json
import os
import re
import subprocess
import sys
import urllib.request
import zipfile

SCHEMA = 1
APP = "OrcaZoukei"
DEFAULT_REPO = "opoeta/OrcaSlicer-Zoukei"
# Not part of the release: debug leftovers and files the app/updater writes at run time.
DEFAULT_EXCLUDES = ["*.pdb", "*.ilk", "*.exp", "zoukei-installed.json", "*/__pycache__/*", "__pycache__/*", "*.pyc"]
GITHUB_ASSET_LIMIT = 2 * 1024 ** 3 - 1
VERSION_RE = re.compile(r"^[0-9]+(\.[0-9]+){1,3}(-[0-9A-Za-z.]+)?$")


def sha256_file(path, bufsize=1 << 20):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while True:
            b = f.read(bufsize)
            if not b:
                break
            h.update(b)
    return h.hexdigest()


def safe_relpath(rel):
    """Same rules the client applies: relative, forward slashes, no '..', no drive, no control chars."""
    if not rel or rel.startswith("/") or "\\" in rel or ":" in rel:
        return False
    if any(ord(c) < 32 for c in rel):
        return False
    return all(p not in ("", ".", "..") and p[-1] not in ". " for p in rel.split("/"))


def scan_tree(tree, excludes):
    files = {}
    for root, dirs, names in os.walk(tree):
        dirs.sort()
        for n in sorted(names):
            full = os.path.join(root, n)
            rel = os.path.relpath(full, tree).replace(os.sep, "/")
            if any(fnmatch.fnmatch(rel, pat) for pat in excludes):
                continue
            if os.path.islink(full) or not os.path.isfile(full):
                continue
            if not safe_relpath(rel):
                raise SystemExit("unsafe file name in the tree: %r" % rel)
            files[rel] = {"size": os.path.getsize(full), "sha256": sha256_file(full)}
    return files


def write_zip(path, tree, rels, level=6):
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED, compresslevel=level, allowZip64=True) as z:
        for rel in sorted(rels):
            z.write(os.path.join(tree, rel.replace("/", os.sep)), rel)


def load_manifest(path):
    with open(path, "r", encoding="utf-8") as f:
        m = json.load(f)
    if m.get("schema") != SCHEMA or m.get("app") != APP or "files" not in m or "version" not in m:
        raise SystemExit("not an Orca Zoukei manifest: %s" % path)
    return m


def fetch_previous(repo, platform, count, dest_dir, current_version):
    """Download the manifests of the latest `count` published releases (public API, read only)."""
    url = "https://api.github.com/repos/%s/releases?per_page=30" % repo
    req = urllib.request.Request(url, headers={"Accept": "application/vnd.github+json", "User-Agent": "zoukei-make-release"})
    with urllib.request.urlopen(req, timeout=30) as r:
        releases = json.load(r)
    found = []
    for rel in releases:
        if rel.get("draft"):
            continue
        for a in rel.get("assets", []):
            if a["name"] == "manifest-%s.json" % platform:
                found.append(a["browser_download_url"])
    os.makedirs(dest_dir, exist_ok=True)
    paths = []
    for i, u in enumerate(found):
        if len(paths) >= count:
            break
        if not u.startswith("https://github.com/%s/releases/download/" % repo):
            continue
        p = os.path.join(dest_dir, "prev-%d.json" % i)
        req = urllib.request.Request(u, headers={"User-Agent": "zoukei-make-release"})
        with urllib.request.urlopen(req, timeout=60) as r, open(p, "wb") as f:
            f.write(r.read())
        if load_manifest(p)["version"] != current_version:
            paths.append(p)
    return paths


def build(tree, version, out, platform, previous, excludes, exe_name="orca-slicer.exe", zip_level=6, log=print):
    if not VERSION_RE.match(version):
        raise SystemExit("bad version %r (use e.g. 2026.10.17 or 2026.10.17.1)" % version)
    if not os.path.isfile(os.path.join(tree, exe_name)):
        raise SystemExit("%s not found in %s: is --tree the install tree (build/OrcaSlicer)?" % (exe_name, tree))
    os.makedirs(out, exist_ok=True)
    log("hashing %s ..." % tree)
    files = scan_tree(tree, excludes)
    log("%d files, %.1f MB" % (len(files), sum(f["size"] for f in files.values()) / 1e6))

    full_name = "%s-%s-%s.zip" % (APP, version, platform)
    full_path = os.path.join(out, full_name)
    log("writing %s ..." % full_name)
    write_zip(full_path, tree, files.keys(), zip_level)
    manifest = {
        "schema": SCHEMA, "app": APP, "version": version, "platform": platform, "exe": exe_name,
        "files": {k: [v["size"], v["sha256"]] for k, v in sorted(files.items())},
        "full": {"asset": full_name, "size": os.path.getsize(full_path), "sha256": sha256_file(full_path)},
        "deltas": [],
    }
    assets = [full_name]
    seen_from = set()
    for pm in previous:
        pv = pm["version"]
        if pv == version or pv in seen_from or pm.get("platform") != platform:
            continue
        seen_from.add(pv)
        old = {k: tuple(v) for k, v in pm["files"].items()}
        changed = sorted(k for k, v in files.items() if old.get(k) != (v["size"], v["sha256"]))
        removed = sorted(k for k in old if k not in files)
        name = "delta-%s-%s-%s.zip" % (pv, version, platform)
        path = os.path.join(out, name)
        log("delta from %s: %d changed/new, %d removed -> %s" % (pv, len(changed), len(removed), name))
        write_zip(path, tree, changed, zip_level)
        size = os.path.getsize(path)
        if size >= manifest["full"]["size"]:
            log("  (not smaller than the full zip, skipped)")
            os.remove(path)
            continue
        manifest["deltas"].append({"from": pv, "asset": name, "size": size, "sha256": sha256_file(path),
                                   "files": changed, "removed": removed})
        assets.append(name)
    mname = "manifest-%s.json" % platform
    mpath = os.path.join(out, mname)
    with open(mpath, "w", encoding="utf-8", newline="\n") as f:
        json.dump(manifest, f, ensure_ascii=False, separators=(",", ":"))
    assets.append(mname)
    sums = os.path.join(out, "SHA256SUMS.txt")
    with open(sums, "w", encoding="utf-8", newline="\n") as f:
        for a in assets:
            f.write("%s  %s\n" % (sha256_file(os.path.join(out, a)), a))
    assets.append("SHA256SUMS.txt")
    for a in assets:
        if os.path.getsize(os.path.join(out, a)) > GITHUB_ASSET_LIMIT:
            log("WARNING: %s is over the 2 GiB GitHub asset limit" % a)
    return manifest, assets


def git_head(cwd):
    try:
        return subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=cwd, text=True).strip()
    except Exception:
        return ""


def gh_command(repo, version, out, assets, prerelease, notes_file, target):
    q = lambda s: '"%s"' % s.replace('"', '\\"')
    parts = ["gh release create", q("v" + version), "--repo", repo, "--title", q("Orca Zoukei " + version)]
    if target:
        parts += ["--target", target]
    if prerelease:
        parts.append("--prerelease")
    parts += ["--notes-file", q(notes_file)] if notes_file else ["--notes", q("Orca Zoukei " + version)]
    parts += [q(os.path.join(out, a)) for a in assets]
    return " ".join(parts)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--tree", required=True, help="install tree to package (build/OrcaSlicer, after the pt_BR .mo was regenerated)")
    ap.add_argument("--version", required=True, help="release version, e.g. 2026.10.17 (tag is v<version>)")
    ap.add_argument("--out", help="output directory (default dist/release-<version>)")
    ap.add_argument("--platform", default="windows-x64")
    ap.add_argument("--repo", default=DEFAULT_REPO)
    ap.add_argument("--previous-manifest", action="append", default=[], help="manifest of an earlier release (repeatable)")
    ap.add_argument("--fetch-previous", type=int, default=0, help="download the manifests of the last N published releases")
    ap.add_argument("--max-deltas", type=int, default=3)
    ap.add_argument("--exclude", action="append", default=[], help="extra fnmatch pattern to leave out (repeatable)")
    ap.add_argument("--prerelease", action="store_true", help="mark the release as a pre-release (beta channel)")
    ap.add_argument("--notes-file", help="release notes (Markdown) shown in the update dialog")
    ap.add_argument("--exe", default="orca-slicer.exe")
    ap.add_argument("--zip-level", type=int, default=6)
    ap.add_argument("--target", default=None, help="commit for the tag (default: git HEAD of the current directory)")
    a = ap.parse_args(argv)

    out = a.out or os.path.join("dist", "release-" + a.version)
    prev_paths = list(a.previous_manifest)
    if a.fetch_previous:
        prev_paths += fetch_previous(a.repo, a.platform, a.fetch_previous, os.path.join(out, "_prev"), a.version)
    previous = sorted((load_manifest(p) for p in prev_paths), key=lambda m: [int(x) if x.isdigit() else 0 for x in re.split(r"[.-]", m["version"])], reverse=True)
    previous = previous[:a.max_deltas]
    manifest, assets = build(a.tree, a.version, out, a.platform, previous, DEFAULT_EXCLUDES + a.exclude, a.exe, a.zip_level)

    target = a.target if a.target is not None else git_head(".")
    print("\nDone. %d files, full zip %.0f MB, %d delta(s). Nothing was published.\n" % (
        len(manifest["files"]), manifest["full"]["size"] / 1e6, len(manifest["deltas"])))
    print("Publish (OWNER ONLY, after pushing the commit that built this tree):\n")
    print(gh_command(a.repo, a.version, out, assets, a.prerelease, a.notes_file, target))
    return 0


if __name__ == "__main__":
    sys.exit(main())
