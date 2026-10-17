#!/usr/bin/env python3
"""Tests of make_release.py and of zoukei-updater.exe (the Windows part is skipped when the exe is not given).

    python tools/zoukei/test_zoukei_tools.py [--updater build/OrcaSlicer/zoukei-updater.exe]

Nothing here touches GitHub or the real install: it works on temporary trees.
"""
import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import make_release as mr  # noqa: E402

UPDATER = None


def write(root, rel, data):
    p = os.path.join(root, rel.replace("/", os.sep))
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, "wb") as f:
        f.write(data if isinstance(data, bytes) else data.encode())


def make_tree(root, variant):
    write(root, "orca-slicer.exe", b"EXE" * 100 + bytes([variant]))
    write(root, "OrcaSlicer.dll", os.urandom(5000) if variant == 0 else b"dll-v%d" % variant * 700)
    write(root, "resources/same.txt", "never changes")
    write(root, "resources/profiles/a.json", "a" * 300)
    write(root, "resources/i18n/pt_BR/OrcaSlicer.mo", b"mo%d" % variant)
    write(root, "zoukei-updater.exe", b"UPD")
    if variant == 0:
        write(root, "resources/old_only.txt", "removed later")
    else:
        write(root, "resources/new_only.txt", "added in v%d" % variant)
    write(root, "resources/ünïcode ñame.txt", "unicode path")
    write(root, "debug.pdb", b"pdb")  # excluded


def read_tree(root):
    out = {}
    for d, _, names in os.walk(root):
        for n in names:
            p = os.path.join(d, n)
            with open(p, "rb") as fh:
                out[os.path.relpath(p, root).replace(os.sep, "/")] = hashlib.sha256(fh.read()).hexdigest()
    return out


class MakeReleaseTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp(prefix="zoukei-test-")
        cls.t0, cls.t1, cls.t2 = [os.path.join(cls.tmp, "tree%d" % i) for i in range(3)]
        for i, t in enumerate((cls.t0, cls.t1, cls.t2)):
            make_tree(t, i)
        quiet = lambda *_: None
        cls.m0, _ = mr.build(cls.t0, "2026.10.10", os.path.join(cls.tmp, "r0"), "windows-x64", [], mr.DEFAULT_EXCLUDES, log=quiet)
        cls.m1, _ = mr.build(cls.t1, "2026.10.11", os.path.join(cls.tmp, "r1"), "windows-x64", [cls.m0], mr.DEFAULT_EXCLUDES, log=quiet)
        cls.m2, cls.assets2 = mr.build(cls.t2, "2026.10.12", os.path.join(cls.tmp, "r2"), "windows-x64", [cls.m1, cls.m0], mr.DEFAULT_EXCLUDES, log=quiet)
        cls.out2 = os.path.join(cls.tmp, "r2")

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def test_manifest_lists_files_and_excludes_pdb(self):
        self.assertIn("orca-slicer.exe", self.m2["files"])
        self.assertNotIn("debug.pdb", self.m2["files"])
        for rel, (size, sha) in self.m2["files"].items():
            with open(os.path.join(self.t2, rel.replace("/", os.sep)), "rb") as fh:
                data = fh.read()
            self.assertEqual(size, len(data))
            self.assertEqual(sha, hashlib.sha256(data).hexdigest())

    def test_full_zip_matches_manifest(self):
        full = os.path.join(self.out2, self.m2["full"]["asset"])
        self.assertEqual(self.m2["full"]["sha256"], mr.sha256_file(full))
        with zipfile.ZipFile(full) as z:
            self.assertEqual(sorted(z.namelist()), sorted(self.m2["files"]))

    def test_delta_contains_only_changes_and_removals(self):
        d = {x["from"]: x for x in self.m2["deltas"]}
        self.assertEqual(set(d), {"2026.10.11", "2026.10.10"})
        d1 = d["2026.10.11"]
        self.assertEqual(d1["files"], ["OrcaSlicer.dll", "orca-slicer.exe", "resources/i18n/pt_BR/OrcaSlicer.mo", "resources/new_only.txt"])
        self.assertEqual(d1["removed"], [])
        d0 = d["2026.10.10"]
        self.assertIn("resources/old_only.txt", d0["removed"])
        with zipfile.ZipFile(os.path.join(self.out2, d1["asset"])) as z:
            self.assertEqual(sorted(z.namelist()), d1["files"])
        self.assertEqual(d1["sha256"], mr.sha256_file(os.path.join(self.out2, d1["asset"])))
        self.assertLess(d1["size"], self.m2["full"]["size"])

    def test_safe_relpath(self):
        for bad in ["", "/a", "a/../b", "..", "a\\b", "C:/x", "a//b", "a/./b", "a\x01b", "a/b."]:
            self.assertFalse(mr.safe_relpath(bad), bad)
        for good in ["a", "a/b.txt", "resources/ünï.txt"]:
            self.assertTrue(mr.safe_relpath(good), good)

    def test_gh_command_is_printed_not_run(self):
        cmd = mr.gh_command("opoeta/OrcaZoukei", "2026.10.12", self.out2, self.assets2, False, None, "abc123")
        self.assertTrue(cmd.startswith('gh release create "v2026.10.12" --repo opoeta/OrcaZoukei'))
        self.assertIn("manifest-windows-x64.json", cmd)


@unittest.skipUnless(sys.platform == "win32", "Windows only")
class UpdaterTests(unittest.TestCase):
    def setUp(self):
        if not UPDATER:
            self.skipTest("no --updater given")
        self.tmp = tempfile.mkdtemp(prefix="zoukei-upd-")
        self.v1, self.v2 = os.path.join(self.tmp, "v1"), os.path.join(self.tmp, "v2")
        make_tree(self.v1, 1)
        make_tree(self.v2, 2)
        self.install = os.path.join(self.tmp, "install")
        shutil.copytree(self.v1, self.install)
        write(self.install, "user_file.txt", "belongs to the user")  # not in any manifest
        self.stage = os.path.join(self.tmp, "work", "files")
        os.makedirs(self.stage)

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def plan(self, puts, dels, pid=None, extra=""):
        lines = ["zoukei-plan\t1", "version\t2026.10.12", "install\t" + self.install, "stage\t" + self.stage,
                 "backup\t" + os.path.join(self.tmp, "work", "backup"), "log\t" + os.path.join(self.tmp, "work", "update.log"),
                 "exe\torca-slicer.exe", "relaunch\t0"]
        if pid:
            lines.append("pid\t%d" % pid)
        for rel in puts:
            with open(os.path.join(self.v2, rel.replace("/", os.sep)), "rb") as fh:
                data = fh.read()
            dst = os.path.join(self.stage, rel.replace("/", os.sep))
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            with open(dst, "wb") as fh:
                fh.write(data)
            lines.append("put\t%s\t%d\t%s" % (rel, len(data), hashlib.sha256(data).hexdigest()))
        lines += ["del\t" + d for d in dels]
        p = os.path.join(self.tmp, "work", "plan.txt")
        with open(p, "w", encoding="utf-8", newline="\n") as fh:
            fh.write("\n".join(lines) + "\n" + extra)
        return p

    def run_updater(self, plan):
        return subprocess.run([UPDATER, plan, "--quiet"], timeout=120).returncode

    def test_success_applies_and_keeps_user_files(self):
        puts = ["orca-slicer.exe", "OrcaSlicer.dll", "resources/new_only.txt", "resources/i18n/pt_BR/OrcaSlicer.mo", "resources/ünïcode ñame.txt"]
        rc = self.run_updater(self.plan(puts, ["resources/profiles/a.json"]))
        self.assertEqual(rc, 0)
        now = read_tree(self.install)
        want = read_tree(self.v2)
        for rel in puts:
            self.assertEqual(now[rel], want[rel], rel)
        self.assertNotIn("resources/profiles/a.json", now)
        self.assertIn("user_file.txt", now)
        self.assertFalse(os.path.exists(self.stage))  # cleaned up

    def test_bad_hash_changes_nothing(self):
        p = self.plan(["orca-slicer.exe", "OrcaSlicer.dll"], [])
        with open(os.path.join(self.stage, "OrcaSlicer.dll"), "ab") as f:
            f.write(b"tampered")
        before = read_tree(self.install)
        self.assertEqual(self.run_updater(p), 2)
        self.assertEqual(read_tree(self.install), before)

    def test_unsafe_path_in_plan_is_rejected(self):
        p = self.plan(["orca-slicer.exe"], [], extra="put\t../evil.txt\t1\t" + "0" * 64 + "\n")
        before = read_tree(self.install)
        self.assertEqual(self.run_updater(p), 3)
        self.assertEqual(read_tree(self.install), before)
        self.assertFalse(os.path.exists(os.path.join(self.tmp, "evil.txt")))

    def test_locked_file_rolls_back_everything(self):
        import ctypes
        from ctypes import wintypes
        k32 = ctypes.WinDLL("kernel32", use_last_error=True)
        k32.CreateFileW.restype = wintypes.HANDLE
        k32.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, wintypes.LPVOID, wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
        # the file that is replaced LAST is held open without sharing, so the earlier ones were already applied
        puts = ["OrcaSlicer.dll", "orca-slicer.exe", "resources/new_only.txt", "resources/profiles/a.json"]
        p = self.plan(puts, [])
        before = read_tree(self.install)
        h = k32.CreateFileW(os.path.join(self.install, "resources", "profiles", "a.json"), 0x80000000, 0, None, 3, 0x80, None)
        self.assertNotEqual(h, wintypes.HANDLE(-1).value)
        try:
            rc = self.run_updater(p)  # retries ~30 s on the sharing violation, then rolls back
        finally:
            k32.CloseHandle(h)
        self.assertEqual(rc, 1)
        self.assertEqual(read_tree(self.install), before)

    def test_waits_for_the_process_to_exit(self):
        proc = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(4)"])
        p = self.plan(["orca-slicer.exe"], [], pid=proc.pid)
        import time
        t = time.time()
        rc = self.run_updater(p)
        self.assertEqual(rc, 0)
        self.assertGreaterEqual(time.time() - t, 3)
        proc.wait()


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--updater")
    args, rest = ap.parse_known_args()
    UPDATER = os.path.abspath(args.updater) if args.updater else None
    unittest.main(argv=[sys.argv[0]] + rest, verbosity=2)
