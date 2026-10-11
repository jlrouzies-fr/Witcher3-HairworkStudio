r"""embedded_python - the private Python the Hairwork Studio release bundles (players install no Python).

  py desktop_studio\packaging\embedded_python.py [--out <folder>]        # builds <folder>\python (default build\python_embed\runtime\python)

What it makes: the official CPython "Windows embeddable package (64-bit)" from python.org (PSF, signed by the PSF), with
site-packages switched on and the pipeline's pip dependencies vendored into Lib\site-packages (win_amd64 wheels, versions pinned
below). The downloads are cached in build\python_embed\ (the zip, checked against the pinned SHA-256, and the wheels), so a
rebuild is deterministic and works offline once the caches exist. The Studio exe uses <exe folder>\python\python.exe when it
exists (settings can override); dev builds keep using the "py" launcher.
Nothing from here is committed: the public repository carries only this script (export_public.py leaves out build\).
Also returns the licence files (CPython + every vendored package) for the release's licenses\ folder.
"""
import argparse
import hashlib
import os
import shutil
import subprocess
import sys
import urllib.request
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
CACHE = os.path.join(ROOT, 'build', 'python_embed')

PY_VERSION = '3.13.0'        # same minor as the dev pipeline (tested with 3.13)
PY_ZIP = 'python-%s-embed-amd64.zip' % PY_VERSION
PY_URL = 'https://www.python.org/ftp/python/%s/%s' % (PY_VERSION, PY_ZIP)
PY_SHA256 = '01c32d0737432240adcf0bbc1d32327f0976d3a1e1427774bc8febc8f1c03111'   # of the file python.org serves, 2026-10-11
PY_TAG = '313'
# pip dependencies of the Studio's Python tools, pinned (the versions the pipeline was tested with)
PINS = ['numpy==2.5.2', 'pillow==12.3.0', 'scipy==1.18.1', 'lz4==4.4.5']
# paths (relative to python\) the tools import from; the release folder is the repo root of the bundled pipeline
PTH_PATHS = ['python%s.zip' % PY_TAG, '.', 'Lib/site-packages', '..', '../tools', '../tools/hairworks',
             '../desktop_studio/tools', '../core']
# not needed at run time: tests, bytecode, C headers / import libs, type stubs, console scripts
STRIP_DIRS = {'tests', '__pycache__', 'include', 'bin'}
STRIP_EXT = ('.pyc', '.h', '.lib', '.pyi', '.c', '.cpp', '.pxd', '.pyx')


def sha256(p):
    h = hashlib.sha256()
    with open(p, 'rb') as f:
        for b in iter(lambda: f.read(1 << 20), b''):
            h.update(b)
    return h.hexdigest()


def fetch_zip():
    os.makedirs(CACHE, exist_ok=True)
    p = os.path.join(CACHE, PY_ZIP)
    if not os.path.exists(p):
        print('downloading', PY_URL)
        urllib.request.urlretrieve(PY_URL, p)
    if sha256(p) != PY_SHA256:
        raise SystemExit('%s: SHA-256 does not match the pinned value (delete it to download again)' % p)
    return p


def fetch_wheels():
    wh = os.path.join(CACHE, 'wheels')
    os.makedirs(wh, exist_ok=True)
    have = ' '.join(os.listdir(wh)).lower()
    if not all(p.split('==')[0].lower() in have for p in PINS):
        subprocess.run([sys.executable, '-m', 'pip', 'download', '--only-binary=:all:', '--platform', 'win_amd64',
                        '--python-version', '3.13', '--implementation', 'cp', '--abi', 'cp313', '--no-deps', '-d', wh] + PINS, check=True)
    return wh


def build(out_dir):
    """-> the python folder; licence files: see licenses_for()"""
    z = fetch_zip()
    wh = fetch_wheels()
    if os.path.isdir(out_dir):
        shutil.rmtree(out_dir)
    os.makedirs(out_dir)
    with zipfile.ZipFile(z) as zf:
        zf.extractall(out_dir)
    # ._pth: site-packages on, our folders on the path (a ._pth file makes the interpreter ignore PYTHONPATH / the script folder)
    pth = os.path.join(out_dir, 'python%s._pth' % PY_TAG)
    open(pth, 'w', encoding='ascii', newline='\r\n').write('\n'.join(PTH_PATHS) + '\nimport site\n')
    sp = os.path.join(out_dir, 'Lib', 'site-packages')
    os.makedirs(sp)
    wheels = sorted(os.path.join(wh, f) for f in os.listdir(wh) if f.endswith('.whl'))
    # wheels are zips: unpack them ourselves (no pip / network needed on a rebuild)
    for w in wheels:
        with zipfile.ZipFile(w) as zf:
            zf.extractall(sp)
    for dp, dn, fn in os.walk(sp, topdown=True):
        for d in list(dn):
            if d in STRIP_DIRS and not d.endswith('.dist-info'):
                shutil.rmtree(os.path.join(dp, d))
                dn.remove(d)
        for f in fn:
            if f.endswith(STRIP_EXT):
                os.remove(os.path.join(dp, f))
    for d in os.listdir(sp):                       # pip bookkeeping not needed at run time (licences are kept by licenses_for)
        if d.endswith('.dist-info'):
            for f in ('RECORD', 'INSTALLER', 'REQUESTED', 'WHEEL', 'DELVEWHEEL', 'sboms'):
                p = os.path.join(sp, d, f)
                if os.path.isdir(p):
                    shutil.rmtree(p)
                elif os.path.exists(p):
                    os.remove(p)
    return out_dir


def licenses_for(py_dir):
    """-> [(abs path, relative path under licenses)]: CPython LICENSE + each vendored package licence files"""
    out = [(os.path.join(py_dir, 'LICENSE.txt'), 'python/LICENSE_CPython_PSF.txt')]
    sp = os.path.join(py_dir, 'Lib', 'site-packages')
    for d in sorted(os.listdir(sp)):
        if not d.endswith('.dist-info'):
            continue
        pkg = d[:-len('.dist-info')]
        for dp, _, fns in os.walk(os.path.join(sp, d)):
            for f in fns:
                if f.upper().startswith(('LICENSE', 'LICENCE', 'COPYING', 'NOTICE', 'AUTHORS')):
                    p = os.path.join(dp, f)
                    rel = os.path.relpath(p, os.path.join(sp, d))
                    out.append((p, pkg + '/' + rel.replace(chr(92), '/')))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', default=os.path.join(CACHE, 'runtime', 'python'))
    a = ap.parse_args()
    p = build(a.out)
    size = sum(os.path.getsize(os.path.join(dp, f)) for dp, _, fs in os.walk(p) for f in fs)
    print('embedded Python %s -> %s (%.1f MB, %d licence files)' % (PY_VERSION, p, size / 1e6, len(licenses_for(p))))


if __name__ == '__main__':
    main()
