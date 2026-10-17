"""End-to-end test of the Orca Zoukei updater against a LOCAL fake release (no GitHub, temporary --datadir, no printers).

    python tools/zoukei/e2e_update_test.py --tree build/OrcaSlicer --work D:/scratch/zoukei-e2e [A] [B] [C] [D]

Needs ~3 GB free in --work (copies of the install tree). It starts the app copy in <work>/inst3 with ORCA_ZOUKEI_UPDATE_URL pointing to
http://127.0.0.1:8099 and ORCA_ZOUKEI_UPDATE_AUTO=1, so the app itself checks, downloads, stages and hands over to zoukei-updater.exe:
  A  delta package          B  corrupted delta (nothing may change)        C  full package (no delta available)
  D  opens the real update dialog and clicks Update and Restart through it (needs a display)
Never uses the real AppData and never contacts GitHub. The app copy does not register the orcaslicer:// handler in test mode.
"""
import argparse, hashlib, json, os, shutil, subprocess, sys, time, tempfile
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import make_release as mr

BASE = r"D:\orca\zoukei-e2e"
SRC = r"D:\orca\k2-wave-main\build\OrcaSlicer"
PORT = 8099
INST = os.path.join(BASE, "inst3")
quiet = lambda *a: None


def tree_hashes(root):
    out = {}
    for d, _, names in os.walk(root):
        for n in names:
            p = os.path.join(d, n)
            rel = os.path.relpath(p, root).replace(os.sep, "/")
            if rel.endswith(".pyc") or "__pycache__" in rel:
                continue
            out[rel] = mr.sha256_file(p)
    return out


def robocopy(src, dst):
    subprocess.run(["robocopy", src, dst, "/MIR", "/NFL", "/NDL", "/NJH", "/NJS", "/NP", "/MT:16"], check=False, stdout=subprocess.DEVNULL)


def write(p, text):
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, "w", newline="") as f:
        f.write(text)


def prepare_bases():
    inst0 = os.path.join(BASE, "inst0")
    tree18 = os.path.join(BASE, "tree18")
    if not os.path.isdir(inst0):
        robocopy(SRC, inst0)
        write(os.path.join(inst0, "resources", "zoukei_e2e", "old.txt"), "old file")
    shutil.rmtree(tree18, ignore_errors=True)
    robocopy(inst0, tree18)
    with open(os.path.join(tree18, "LICENSE.txt"), "a") as f:
        f.write("\nchanged in 18\n")
    os.remove(os.path.join(tree18, "resources", "zoukei_e2e", "old.txt"))
    write(os.path.join(tree18, "resources", "zoukei_e2e", "new.txt"), "new file 18")
    return inst0, tree18


def run_scenario(name, release_dirs, inst0, expect_tree, m17, server_args=(), mode="1"):
    print("\n=== scenario", name, flush=True)
    shutil.rmtree(INST, ignore_errors=True)
    robocopy(inst0, INST)
    with open(os.path.join(INST, "zoukei-installed.json"), "w", encoding="utf-8") as f:
        json.dump(m17, f, separators=(",", ":"))
    datadir = tempfile.mkdtemp(prefix="zoukei-e2e-data-")
    write(os.path.join(datadir, "OrcaSlicer.conf"), json.dumps({"app": {"stealth_mode": "true"}}) + "\r\n")
    srv = subprocess.Popen([sys.executable, r"D:\orca\k2-wave-main\tools\zoukei\fake_release_server.py", "--port", str(PORT)] + list(server_args) + release_dirs,
                           stderr=open(os.path.join(BASE, "server.log"), "w"), stdout=subprocess.DEVNULL)
    time.sleep(1.5)
    env = dict(os.environ, ORCA_ZOUKEI_UPDATE_URL="http://127.0.0.1:%d/releases.json" % PORT, ORCA_ZOUKEI_UPDATE_AUTO=mode)
    before = tree_hashes(INST)
    if expect_tree is None:
        expect_tree = before
    shutil.rmtree(os.path.join(tempfile.gettempdir(), "OrcaZoukeiUpdate"), ignore_errors=True)
    t = time.time()
    p = subprocess.Popen([os.path.join(INST, "orca-slicer.exe"), "--datadir", datadir], env=env, cwd=INST)
    try:
        p.wait(timeout=120)
    except subprocess.TimeoutExpired:
        p.kill()
        print("orca did not exit by itself (killed)")
    print("orca exit code", p.returncode, "after %.1fs" % (time.time() - t), flush=True)
    for _ in range(150):  # the updater runs detached after orca exits
        r = subprocess.run(["tasklist", "/FI", "IMAGENAME eq zoukei-updater.exe"], capture_output=True, text=True).stdout
        if "zoukei-updater.exe" not in r:
            break
        time.sleep(1)
    srv.terminate()
    after = tree_hashes(INST)
    ok = after == expect_tree
    dialog_ran = False
    work = os.path.join(tempfile.gettempdir(), "OrcaZoukeiUpdate")
    for d, _, names in os.walk(work):
        for n in names:
            if n == "update.log":
                print(open(os.path.join(d, n), encoding="utf-8", errors="replace").read())
    for d, _, names in os.walk(datadir):
        for n in names:
            if ".log" in n:
                for line in open(os.path.join(d, n), encoding="utf-8", errors="replace"):
                    if "ZoukeiUpdate" in line or "test hook" in line or "update failed" in line.lower():
                        print("  log:", line.strip()[:240])
                    if "ZoukeiUpdate::prepare" in line and " staged" in line:
                        dialog_ran = True
    if not ok:
        diff = [k for k in set(after) | set(expect_tree) if after.get(k) != expect_tree.get(k)]
        print("DIFF vs expected:", diff[:20])
    if mode == "dialog":  # the dialog was opened, "Update" clicked by the hook, files staged; whether the app could then close is not the point
        ok = dialog_ran
    print("RESULT", name, "OK" if ok else "FAIL", flush=True)
    shutil.rmtree(datadir, ignore_errors=True)
    return ok


def main():
    global BASE, SRC, INST
    ap = argparse.ArgumentParser()
    ap.add_argument("--tree", required=True, help="install tree (build/OrcaSlicer)")
    ap.add_argument("--work", required=True, help="scratch folder, ~3 GB")
    ap.add_argument("scenarios", nargs="*", default=["A", "B", "C"])
    a = ap.parse_args()
    SRC, BASE = os.path.abspath(a.tree), os.path.abspath(a.work)
    INST = os.path.join(BASE, "inst3")
    os.makedirs(BASE, exist_ok=True)
    which = a.scenarios
    inst0, tree18 = prepare_bases()
    for d in ("r17", "r18", "r18full"):
        shutil.rmtree(os.path.join(BASE, d), ignore_errors=True)
    m17, _ = mr.build(inst0, "2026.10.17", os.path.join(BASE, "r17"), "windows-x64", [], mr.DEFAULT_EXCLUDES, zip_level=1, log=print)
    m18, _ = mr.build(tree18, "2026.10.18", os.path.join(BASE, "r18"), "windows-x64", [m17], mr.DEFAULT_EXCLUDES, zip_level=1, log=print)
    base18 = tree_hashes(tree18)
    exp18 = dict(base18)
    exp18["zoukei-installed.json"] = mr.sha256_file(os.path.join(BASE, "r18", "manifest-windows-x64.json"))
    results = {}
    if "A" in which:
        results["A delta"] = run_scenario("A delta", [os.path.join(BASE, "r18")], inst0, exp18, m17)
    if "B" in which:
        results["B corrupt delta (must change nothing)"] = run_scenario("B corrupt", [os.path.join(BASE, "r18")], inst0, None, m17,
                                                                         server_args=["--corrupt", m18["deltas"][0]["asset"]])
    if "C" in which:
        mf, _ = mr.build(tree18, "2026.10.18", os.path.join(BASE, "r18full"), "windows-x64", [], mr.DEFAULT_EXCLUDES, zip_level=1, log=quiet)
        exp = dict(base18)
        exp["zoukei-installed.json"] = mr.sha256_file(os.path.join(BASE, "r18full", "manifest-windows-x64.json"))
        results["C full package"] = run_scenario("C full", [os.path.join(BASE, "r18full")], inst0, exp, m17)
    if "D" in which:
        # the real dialog: offer -> Update -> ready -> Restart; the test hook ends the process afterwards
        results["D dialog"] = run_scenario("D dialog", [os.path.join(BASE, "r18")], inst0, None, m17, mode="dialog")
    print("\nSUMMARY", results)


if __name__ == "__main__":
    main()
