"""NVIDIA SDK guard: keeps NVIDIA HairWorks SDK material out of git and out of any public copy.

The GameWorks EULA (section 8) makes the SDK's source, headers and documentation NVIDIA Confidential Information, and
section 2 allows distributing it only as object code inside the application. This repository must therefore never
contain SDK files, copies of them, or code pasted from them. Checks:

  1. Paths: sources/ (the SDK lives there), known SDK file names (NvHair*, NvCo*, GFSDK_*, HairWorks runtime DLLs,
     compiled shaders, the NVIDIA logos), saved NVIDIA docs.
  2. Markers: NVIDIA's proprietary licence header ("... without an express license agreement from NVIDIA CORPORATION
     is strictly prohibited"), outside the files that only quote NVIDIA's terms.
  3. Copies: when the SDK folder is present, exact copies of any SDK file (SHA-1), and text files that share several
     long lines with SDK sources / headers / docs.

Usage (from the repo root):
  py tools/sdk_guard.py --staged          # pre-commit hook (.githooks/pre-commit): staged files
  py tools/sdk_guard.py --tree <dir>      # a folder, e.g. the public-repo export before pushing it
  py tools/sdk_guard.py --tree <dir> --release   # the Nexus release folder: additionally allows exactly the runtime DLL
                                                 # NvHairWorksDx11.win64.dll and our own compiled *.cso (nothing else of NVIDIA's)
  py tools/sdk_guard.py --history         # every path that was ever committed
  py tools/sdk_guard.py --tracked         # every file git tracks now
Exit code 1 = blocked. There is no override switch on purpose: fix the file list instead.
"""
import argparse
import fnmatch
import hashlib
import json
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SDK = os.path.join(ROOT, 'sources', 'HairWorks-r1_2_1-15-distro')
INDEX = os.path.join(ROOT, 'build', 'sdk_guard_index.json')

DENY_PATHS = ['sources/*', 'analysis/hairworks_docs/*']
DENY_NAMES = ['NvHair*', 'NvCo*', 'GFSDK_*', 'NvHairWorks*.dll', '*.cso', 'developerzone_gameworks_logo*',
              'hairworks_logo*', 'nvidia-logo-header*', '*.apx', '*.furproj']
MARKER = re.compile(rb'license\s+agreement\s+from\s+NVIDIA\s+CORPORATION\s+is\s+strictly\s+prohibited', re.I)
# files that quote NVIDIA's terms on purpose (licences, EULA, this guard)
MARKER_OK = ['tools/sdk_guard.py', 'desktop_studio/packaging/EULA_template.txt', '*LICENSE*', '*NOTICE*', '*EULA*']
# --release (the Nexus folder): the only NVIDIA-named binary it may hold is the runtime DLL (EULA section 2: object code inside the
# application); compiled shaders are our own code. Everything else stays denied.
RELEASE_DLL = 'nvhairworksdx11.win64.dll'
LINES_SKIP = ['tools/sdk_guard.py', 'desktop_studio/third_party/*', 'python/Lib/site-packages/*', 'licenses/*']   # vendored third-party Python packages + their licences (release folder)
# Third-party files that must never ship (AGENTS.md: "Never ship third-party mods or their files"). Checked in --tree mode (the public
# export and the Nexus release folders) by name, by SHA-1 (renamed copies) and by the authoring-path marker the Yennefer HairWorks mod
# left in its redfur. The private dev repo keeps tools/hairworks/tpl/fur02.redfur as a legacy build skeleton (redfur.py, "legacy" mode).
THIRD_PARTY_NAMES = ['fur02.redfur', 'fur_02_wa__yennefer_px.redfur']
THIRD_PARTY_SHA1 = {'9a94e1e9186455afcc8d202e8056da52a66454f0': 'Yennefer HairWorks mod fur (tpl/fur02.redfur)'}
THIRD_PARTY_MARK = b'yennefer_' + b'px.apx'          # importFile string inside every fur made on that skeleton
TEXT_EXT = {'.h', '.hpp', '.c', '.cpp', '.inl', '.hlsl', '.hlsli', '.fx', '.py', '.txt', '.md', '.html', '.htm',
            '.js', '.json', '.ps1', '.bat', '.cmake', '.ws', '.xml', '.csv', '.cs'}
LINE_MIN = 40          # only long, specific lines count
LINES_BLOCK = 5        # this many shared long lines with the SDK = a pasted copy


def norm_lines(data):
    out = set()
    for raw in data.decode('utf-8', 'ignore').splitlines():
        s = ' '.join(raw.split())
        if len(s) >= LINE_MIN and not s.startswith(('#include', '//', '/*', '*', '<', 'import ')):
            out.add(hashlib.sha1(s.encode()).hexdigest()[:16])
    return out


def build_index():
    """SHA-1 of every SDK file + hashed long lines of its text files, cached in build/ (git-ignored)."""
    if not os.path.isdir(SDK):
        return None
    stamp = 0.0
    files = []
    for d, _, fs in os.walk(SDK):
        for f in fs:
            p = os.path.join(d, f)
            files.append(p)
            stamp = max(stamp, os.path.getmtime(p))
    if os.path.exists(INDEX):
        try:
            idx = json.load(open(INDEX))
            if idx.get('stamp') == stamp and idx.get('count') == len(files):
                return {'sha': set(idx['sha']), 'lines': set(idx['lines'])}
        except Exception:
            pass
    sha, lines = set(), set()
    for p in files:
        try:
            if os.path.getsize(p) > 64 << 20:
                continue
            data = open(p, 'rb').read()
        except OSError:
            continue
        sha.add(hashlib.sha1(data).hexdigest())
        if os.path.splitext(p)[1].lower() in TEXT_EXT:
            lines |= norm_lines(data)
    os.makedirs(os.path.dirname(INDEX), exist_ok=True)
    json.dump({'stamp': stamp, 'count': len(files), 'sha': sorted(sha), 'lines': sorted(lines)}, open(INDEX, 'w'))
    return {'sha': sha, 'lines': lines}


def match_any(path, pats):
    name = path.rsplit('/', 1)[-1]
    return any(fnmatch.fnmatch(path, p) or fnmatch.fnmatch(name, p) for p in pats)


def check(path, data, idx, release=False, tree=False):
    """-> reason string or None. path uses '/' and is relative to the repo (or export) root."""
    if tree:
        if match_any(path, THIRD_PARTY_NAMES):
            return 'third-party file (never ship): ' + path.rsplit('/', 1)[-1]
        if data is not None:
            h = hashlib.sha1(data).hexdigest()
            if h in THIRD_PARTY_SHA1:
                return 'third-party file (never ship): ' + THIRD_PARTY_SHA1[h]
            if THIRD_PARTY_MARK in data and path != 'tools/sdk_guard.py':
                return 'contains the third-party authoring path marker (built on the Yennefer mod skeleton)'
    if release:
        name = path.rsplit('/', 1)[-1].lower()
        if name == RELEASE_DLL:
            return None if path.lower() == RELEASE_DLL else 'the runtime DLL must sit at the release root'
        if name.endswith('.cso'):
            # our compiled shader: allowed unless it is byte-identical to an SDK file
            if data is not None and idx and hashlib.sha1(data).hexdigest() in idx['sha']:
                return 'exact copy of an NVIDIA SDK file'
            return None if data is None or data[:4] == b'DXBC' else 'not a compiled shader'
    if match_any(path, DENY_PATHS):
        return 'NVIDIA SDK / docs location'
    if match_any(path, DENY_NAMES):
        return 'NVIDIA SDK file name'
    if data is None:
        return None
    if not match_any(path, MARKER_OK) and MARKER.search(data):
        return "NVIDIA proprietary licence header ('... strictly prohibited')"
    if idx:
        if hashlib.sha1(data).hexdigest() in idx['sha']:
            return 'exact copy of an NVIDIA SDK file'
        # third-party libraries (Dear ImGui ...) share boilerplate with the SDK samples: exact copies only
        if os.path.splitext(path)[1].lower() in TEXT_EXT and not match_any(path, LINES_SKIP):
            n = len(norm_lines(data) & idx['lines'])
            if n >= LINES_BLOCK:
                return '%d long lines identical to NVIDIA SDK sources/docs' % n
    return None


def git(*args):
    return subprocess.run(['git', '-C', ROOT] + list(args), capture_output=True, check=True).stdout


def main():
    ap = argparse.ArgumentParser()
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument('--staged', action='store_true')
    g.add_argument('--tree')
    ap.add_argument('--release', action='store_true', help='with --tree: Nexus release folder rules (runtime DLL + our .cso allowed)')
    g.add_argument('--history', action='store_true')
    g.add_argument('--tracked', action='store_true', help='every file git currently tracks (working-tree content)')
    a = ap.parse_args()
    idx = build_index()
    bad = []
    if a.tracked:
        for n in [x for x in git('ls-files', '-z').decode().split('\0') if x]:
            p = os.path.join(ROOT, n)
            data = open(p, 'rb').read() if os.path.isfile(p) and os.path.getsize(p) <= 64 << 20 else None
            r = check(n, data, idx)
            if r:
                bad.append((n, r))
    elif a.staged:
        names = [n for n in git('diff', '--cached', '--name-only', '--diff-filter=ACMR', '-z').decode().split('\0') if n]
        for n in names:
            try:
                data = git('show', ':' + n)
            except subprocess.CalledProcessError:
                data = None
            r = check(n, data, idx)
            if r:
                bad.append((n, r))
    elif a.tree:
        top = os.path.abspath(a.tree)
        for d, dirs, fs in os.walk(top):
            dirs[:] = [x for x in dirs if x != '.git']
            for f in fs:
                p = os.path.join(d, f)
                rel = os.path.relpath(p, top).replace('\\', '/')
                data = open(p, 'rb').read() if os.path.getsize(p) <= 64 << 20 else None
                r = check(rel, data, idx, a.release, tree=True)
                if r:
                    bad.append((rel, r))
    else:
        for n in sorted(set(git('log', '--all', '--name-only', '--format=').decode().split('\n'))):
            if n and (match_any(n, DENY_PATHS) or match_any(n, DENY_NAMES)):
                bad.append((n, 'was committed (path / name)'))
    if not idx:
        print('sdk_guard: SDK folder not found, copy check skipped (path / name / licence-header checks only)')
    if bad:
        print('sdk_guard: BLOCKED - NVIDIA SDK material (GameWorks EULA section 8, confidential):')
        for n, r in bad:
            print('  %s  <- %s' % (n, r))
        return 1
    print('sdk_guard: OK')
    return 0


if __name__ == '__main__':
    sys.exit(main())
