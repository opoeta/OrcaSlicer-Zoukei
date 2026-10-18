"""Syntax/warning check of the fork's C++ files with clang-cl, the way CI builds them (/W4 -Werror plus the
exceptions of the top-level CMakeLists.txt), without a full clang build.

    python tools/zoukei/clang_check.py [--base <commit>] [--jobs N] [files...]

Flags (include dirs, defines, external includes) come from the MSVC build's .vcxproj files in build/, so run it
after a normal build_win.bat configure. Only Windows code paths are checked (no Linux/macOS #ifdefs)."""
import argparse
import concurrent.futures as cf
import os
import re
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
BUILD = os.path.join(ROOT, 'build')
CLANG = r'C:\Program Files\LLVM\bin\clang-cl.exe'

WARN = ['/W4', '-Werror', '-Wno-unused-parameter', '-Wno-ignored-qualifiers', '-Wno-missing-field-initializers',
        '-Wno-reorder', '-Wno-sign-compare', '-Wno-misleading-indentation', '-Wno-switch', '-Wno-unused-function',
        '-Wno-unused-variable', '-Wno-unused-but-set-variable', '-Wno-unused-label', '-Wno-unused-local-typedefs',
        '-Wno-deprecated-declarations', '-Wno-ignored-attributes', '-Wno-error=enum-constexpr-conversion',
        '-Wno-unknown-warning-option', '-Wno-microsoft-include', '-Wno-pragma-pack']

PROJECTS = {
    'src/libslic3r/': 'src/libslic3r/libslic3r.vcxproj',
    'src/libvgcode/': 'src/libvgcode/libvgcode.vcxproj',
    'src/slic3r/': 'src/slic3r/libslic3r_gui.vcxproj',
    'src/': 'src/OrcaSlicer.vcxproj',
}


def project_flags(vcxproj):
    s = open(vcxproj, encoding='utf-8').read()
    blk = s[s.find("<ItemDefinitionGroup Condition=\"'$(Configuration)|$(Platform)'=='Release|x64'\""):]
    blk = blk[:blk.find('</ItemDefinitionGroup>')]
    def tag(t):
        m = re.search('<%s>(.*?)</%s>' % (t, t), blk, re.S)
        return m.group(1) if m else ''
    flags = []
    for d in tag('AdditionalIncludeDirectories').split(';'):
        if d and '%(' not in d:
            flags.append('-imsvc' + d if (os.sep + 'deps' in d or '/deps' in d) else '-I' + d)
    for d in tag('PreprocessorDefinitions').split(';'):
        if d and '%(' not in d:
            flags.append('-D' + d)
    for d in re.findall(r'/external:I "([^"]+)"', tag('AdditionalOptions')):
        norm = d.replace('\\', '/')
        if norm.rstrip('/').endswith('k2-wave-main/src') or norm.endswith('/src/slic3r/..'):
            flags.append('-I' + d)        # the fork's own headers: checked like CI does
        else:
            flags.append('-imsvc' + d)
    return flags


def project_for(path):
    rel = path.replace('\\', '/')
    for prefix, proj in PROJECTS.items():
        if rel.startswith(prefix):
            # pick the zoukei GUI lib when the file is built there (same flags, but keep it honest)
            if proj.endswith('libslic3r_gui.vcxproj'):
                z = os.path.join(BUILD, 'src/slic3r/libslic3r_gui_zoukei.vcxproj')
                if os.path.exists(z) and os.path.basename(rel) in open(z, encoding='utf-8').read():
                    return z, os.path.join(ROOT, 'src/slic3r/pchheader.hpp')
                return os.path.join(BUILD, proj), os.path.join(ROOT, 'src/slic3r/pchheader.hpp')
            pch = os.path.join(ROOT, prefix, 'pchheader.hpp')
            return os.path.join(BUILD, proj), (pch if os.path.exists(pch) else None)
    return None, None


_cache = {}


def check(rel):
    proj, pch = project_for(rel)
    if proj is None or not os.path.exists(proj):
        return rel, 'skip', ''
    if proj not in _cache:
        _cache[proj] = project_flags(proj)
    cmd = [CLANG, '/nologo', '/Zs', '/std:c++17', '/EHsc', '/MD', '/bigobj', '--target=x86_64-pc-windows-msvc',
           '-fms-compatibility-version=19.40'] + WARN + _cache[proj]
    if pch:
        cmd += ['/FI' + pch]
    cmd.append(os.path.join(ROOT, rel))
    p = subprocess.run(cmd, capture_output=True, text=True, encoding='utf-8', errors='replace', cwd=ROOT)
    out = (p.stdout + p.stderr).strip()
    return rel, ('ok' if p.returncode == 0 else 'FAIL'), out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--base', default='3e98bac86c^')
    ap.add_argument('--jobs', type=int, default=max(2, (os.cpu_count() or 4) - 2))
    ap.add_argument('files', nargs='*')
    a = ap.parse_args()
    files = a.files
    if not files:
        out = subprocess.run(['git', 'diff', '--name-only', '--diff-filter=AM', a.base, 'HEAD', '--', '*.cpp'],
                             cwd=ROOT, capture_output=True, text=True).stdout.split()
        out += subprocess.run(['git', 'diff', '--name-only', '--diff-filter=AM', '--', '*.cpp'],
                              cwd=ROOT, capture_output=True, text=True).stdout.split()
        out += subprocess.run(['git', 'ls-files', '--others', '--exclude-standard', '--', 'src/*.cpp'],
                              cwd=ROOT, capture_output=True, text=True).stdout.split()
        files = sorted(set(f for f in out if f.startswith('src/') and not f.startswith('src/dev-utils')))
    bad = 0
    with cf.ThreadPoolExecutor(a.jobs) as ex:
        for rel, status, out in ex.map(check, files):
            if status == 'FAIL':
                bad += 1
                errs = [l for l in out.splitlines() if ' error' in l]
                print('FAIL', rel)
                for l in errs[:12]:
                    print('   ', l[:300])
            elif status == 'skip':
                print('skip', rel)
    print('%d files, %d failing' % (len(files), bad))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
