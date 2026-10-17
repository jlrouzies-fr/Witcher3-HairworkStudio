r"""build_release_hairwork - build the Hairwork Studio release for Nexus (mod 13996). Local only: uploads nothing.

  py tools\build_release_hairwork.py --version 0.1.0 [--build <dir with HairworkStudio.exe>] [--no-package]
                                      [--aio-version 0.1.0] [--no-allinone] [--jobs 3]

Mirrors tools\build_release.py of the Ink & Iron mods. Output (git-ignored): release\HairworkStudio_<version>\
  HairworkStudio_<version>.zip      the Studio (exe, shaders, runtime DLL, bundled pipeline, docs, portraits): the
                                    folder desktop_studio\dist\HairworkStudio-<version> made by
                                    desktop_studio\packaging\package_release.py --no-zip (runs sdk_guard --release)
  HairworkForAllCore_v<core>.zip    the Core mod (core\build_core.py build), required by every exported release mod
  HairworkForAll_AllInOne_<aio>.zip the main mod: RELEASE builds of the shipped characters (tools\build_allinone.py),
                                    skeleton read from the game, never tpl\fur02.redfur
  README.txt, CHANGELOG.txt         (CHANGELOG: one section per version, newest first; the upload posts it)
Steps: 1. Core build + verify. 2. Studio folder (package_release.py). 3. Dev-hook scan of the Studio folder (no test /
debug switches on by default, no private paths, no e-mail, no game files, nothing from NVIDIA but the runtime DLL).
4. Deterministic zips (sorted members, fixed timestamps, deflate level 9). 5. Zip integrity test + content checks.
Upload afterwards with tools\run_nexus_upload_hairwork.bat (the user runs it; --dry-run lists only).
"""
import argparse
import os
import re
import subprocess
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
RELEASE = os.path.join(ROOT, 'release')
ZIP_DT = (2026, 1, 1, 0, 0, 0)          # fixed member timestamp: the same inputs give the same zip
sys.path.insert(0, os.path.join(ROOT, 'core'))

# dev-only names that must not ship enabled: env switches of the test harness are read only by name in the exe
# (checked below as "present but opt-in"), these must not appear in shipped scripts / data at all
FORBIDDEN_TEXT = [b'C:\\Users\\', b'G:\\Games\\',('rouzies.' + 'jeanlaurent').encode(), b'HAIKUSESSION']
FORBIDDEN_PATH_PARTS = ('/results/', '/mods/', '/analysis/', '/sources/', '/.git/', '/portraits_ai/cand/', '__pycache__')
TEXT_EXT = ('.py', '.json', '.txt', '.md', '.html', '.bat', '.ws')
# opt-in test switches the exe reads from the environment; they are off unless the user sets them (documented, harmless)
OPT_IN_ENV = [b'HW_DIRECT_ASSET', b'HW_BONE_STIFF_EMU', b'GROOM_STUDIO_NOACTIVATE']

CHANGELOG = """Hairwork for All changelog
%(aio)s (new release: All In One, Core, Studio)
- Hairwork for All - All In One %(aio)s: rebuilt for 5.0 as release builds. Every character mod requires Hairwork for All - Core and Graphics Control root mode 0 (see README.txt / REQUIREMENTS.txt).
- Rebuilt on a fur skeleton read from the base game (the earlier files used another mod's skeleton): no third-party data in any file.
- Characters: Vesemir (+ scalp texture), Regis, Dandelion, Guillaume, Lambert, Avallac'h, Sheala, Keira, Triss (needs the lore-friendly Triss mod), generic c04ma men.
- Hairwork for All - Core %(core)s: script-only dependency (no game files).
Hairwork Studio changelog
0.1.0 (first release)
- No Python install needed: the Studio bundles its own private Python (python folder). Unzip and run HairworkStudio.exe.
- Hairwork Studio: native Windows editor and simulator for NVIDIA HairWorks hair of Witcher 3 characters (patch 5.0, DX12).
- Builds each character's strand pack from YOUR game install with the bundled pipeline (Python 3 + numpy / Pillow / scipy / lz4); no game files are shipped.
- Real HairWorks 1.2.1 runtime: gravity, stiffness, damping, wind, collision capsules, density map, LOD; motion presets.
- Edit guides (move, shorten, delete, duplicate, mirror, curl / roll / wave), material presets, performance budget, painted portraits picker.
- Export a mod (for your own use) or "Export for release", which needs Hairwork for All - Core (this page) and Graphics Control (Hair root transparency mode = 0).
- Bundled starting points: approved builds (Vesemir, c04ma, Regis, Dandelion, Guillaume) and a plain conversion for every card-hair character.
- Help > User guide opens docs\\guide\\guide.html. docs\\AGENT_GUIDE.md describes the scripted (agent) interface.
Hairwork for All - Core 1.0.0
- Script-only dependency of every Hairwork Studio release export (calls HairworkForAllCore_v1_0). No game files, no bundle.
"""


def read_changelog_header(ver):
    return CHANGELOG


def det_zip(path, files):
    """files: [(abs path, arcname)] -> deterministic zip"""
    tmp = path + '.tmp'
    with zipfile.ZipFile(tmp, 'w', allowZip64=True) as z:
        for p, arc in sorted(files, key=lambda t: t[1].lower()):
            zi = zipfile.ZipInfo(arc.replace('\\', '/'), ZIP_DT)
            zi.create_system = 0
            zi.external_attr = 0o644 << 16
            already = p.lower().endswith(('.jpg', '.png', '.zip', '.cso'))
            zi.compress_type = zipfile.ZIP_STORED if already else zipfile.ZIP_DEFLATED
            if zi.compress_type == zipfile.ZIP_DEFLATED:
                zi.compress_level = zi._compresslevel = 9
            with open(p, 'rb') as src, z.open(zi, 'w', force_zip64=os.path.getsize(p) > 2**31 - 2**24) as dst:
                while True:
                    b = src.read(8 << 20)
                    if not b:
                        break
                    dst.write(b)
    os.replace(tmp, path)


def tree_files(top, prefix):
    out = []
    for dp, _, fns in os.walk(top):
        for fn in fns:
            full = os.path.join(dp, fn)
            out.append((full, prefix + '/' + os.path.relpath(full, top).replace('\\', '/')))
    return out


def scan(folder):
    hits = []
    for dp, _, fns in os.walk(folder):
        for fn in fns:
            p = os.path.join(dp, fn)
            rel = '/' + os.path.relpath(p, folder).replace('\\', '/')
            if rel.startswith(('/python/', '/licenses/')):      # bundled third-party Python runtime (python.org + pip wheels; sdk_guard checks it)
                if '__pycache__' in rel:
                    hits.append('%s: bytecode cache not allowed in a release' % rel)
                continue
            if any(x in rel + '/' for x in FORBIDDEN_PATH_PARTS):
                hits.append('%s: path not allowed in a release' % rel)
            if fn.lower().endswith(TEXT_EXT) and not fn.lower().startswith(('license', 'third_party', 'nvidia')) and 'docs' not in rel.split('/')[1:2]:
                data = open(p, 'rb').read()
                for h in FORBIDDEN_TEXT:
                    if h in data:
                        hits.append('%s: contains %r' % (rel, h.decode('latin1')))
            if fn.lower().endswith('.exe'):
                data = open(p, 'rb').read()
                if ('rouzies.' + 'jeanlaurent').encode() in data:   # split: the address itself never appears in the public repo
                    hits.append('%s: contains the user e-mail' % rel)
    return hits


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--version', default='0.1.0')
    ap.add_argument('--build', default=None, help='folder with the built HairworkStudio.exe (default desktop_studio\\build\\Release)')
    ap.add_argument('--no-package', action='store_true', help='reuse desktop_studio\\dist\\HairworkStudio-<v> as it is')
    ap.add_argument('--aio-version', default='0.1.0', help='version of HairworkForAll_AllInOne_<v>.zip')
    ap.add_argument('--no-allinone', action='store_true')
    ap.add_argument('--jobs', type=int, default=3, help='parallel All In One character builds')
    a = ap.parse_args()
    import hwcore as K
    ver = a.version
    out = os.path.join(RELEASE, 'HairworkStudio_' + ver)
    os.makedirs(out, exist_ok=True)
    py = sys.executable
    # 1. Core
    subprocess.run([py, os.path.join(ROOT, 'core', 'build_core.py'), 'build'], check=True)
    subprocess.run([py, os.path.join(ROOT, 'core', 'build_core.py'), 'verify'], check=True)
    core_dir = os.path.join(ROOT, 'build', 'core', K.CORE_MOD)
    # 2. Studio folder
    stud = os.path.join(ROOT, 'desktop_studio', 'dist', 'HairworkStudio-' + ver)
    if not a.no_package:
        cmd = [py, os.path.join(ROOT, 'desktop_studio', 'packaging', 'package_release.py'), '--version', ver, '--no-zip']
        if a.build:
            cmd += ['--build', a.build]
        subprocess.run(cmd, check=True)
    if not os.path.isdir(stud):
        sys.exit('no Studio folder ' + stud)
    # 3. scan
    hits = scan(stud) + scan(core_dir)
    exe = open(os.path.join(stud, 'HairworkStudio.exe'), 'rb').read()
    en = [h.decode() for h in OPT_IN_ENV if h in exe]
    print('opt-in env switches present in the exe (all off by default): %s' % ', '.join(en) or 'none')
    if hits:
        print('\n'.join(hits))
        sys.exit('release scan failed')
    # 4. zips
    sz = os.path.join(out, 'HairworkStudio_%s.zip' % ver)
    cz = os.path.join(out, 'HairworkForAllCore_v%s.zip' % K.CORE_VERSION)
    det_zip(sz, tree_files(stud, 'HairworkStudio-' + ver))
    det_zip(cz, tree_files(core_dir, K.CORE_MOD))
    # 5. integrity + content checks
    for z in (sz, cz):
        with zipfile.ZipFile(z) as zf:
            bad = zf.testzip()
            if bad:
                sys.exit('zip integrity failed: %s in %s' % (bad, z))
            names = zf.namelist()
            print('%s: %d files, %.1f MB, integrity OK' % (os.path.basename(z), len(names), os.path.getsize(z) / 1e6))
    with zipfile.ZipFile(sz) as zf:
        n = set(zf.namelist())
        pre = 'HairworkStudio-%s/' % ver
        for need in ('HairworkStudio.exe', 'NvHairWorksDx11.win64.dll', 'README.txt', 'requirements.txt', 'LICENSE.txt',
                     'tools/hairworks/hairpipe.py', 'desktop_studio/tools/desk_prep.py', 'core/hwcore.py',
                     'python/python.exe', 'python/python313.dll', 'python/python313._pth', 'python/LICENSE.txt',
                     'python/Lib/site-packages/numpy/__init__.py', 'python/Lib/site-packages/scipy/__init__.py',
                     'python/Lib/site-packages/PIL/__init__.py', 'python/Lib/site-packages/lz4/__init__.py',
                     'licenses/python/LICENSE_CPython_PSF.txt'):
            if pre + need not in n:
                sys.exit('Studio zip lacks ' + need)
        nv = [x for x in n if x.lower().rsplit('/', 1)[-1].startswith('nv') and not x.endswith('NvHairWorksDx11.win64.dll')
              and not x.endswith('NVIDIA_HairWorks_EULA.txt')]
        if nv:
            sys.exit('unexpected NVIDIA-named files: %s' % nv)
    # sdk_guard on the extracted zip contents is the folder check done by package_release; rerun it on the Core
    subprocess.run([py, os.path.join(ROOT, 'tools', 'sdk_guard.py'), '--tree', core_dir], check=True)
    # All In One (the main mod)
    if not a.no_allinone:
        import build_allinone
        build_allinone.build_allinone(out, a.aio_version, K.CORE_VERSION, a.jobs, det_zip, scan)
    # README / CHANGELOG
    open(os.path.join(out, 'CHANGELOG.txt'), 'w', encoding='utf-8').write(CHANGELOG % dict(aio=a.aio_version, core=K.CORE_VERSION))
    open(os.path.join(out, 'README.txt'), 'w', encoding='utf-8').write(
        'Hairwork Studio %s: release files (Nexus witcher3 mod 13996)\n\n'
        'HairworkForAll_AllInOne_%s.zip  the main mod: the character hair mods (mod<Char>Hairworks) as release builds; needs the Core.\n'
        'HairworkStudio_%s.zip          the Studio. Unzip anywhere (write access) and run HairworkStudio.exe: no Python to install.\n'
        'HairworkForAllCore_v%s.zip    the Core mod: copy mod0000HairworkForAllCore into <game>\\mods. Required by every mod exported with\n'
        '                                "Export for release"; install it before testing such a mod.\n'
        'CHANGELOG.txt                  posted as the mod changelog by tools\\nexus_upload_hairwork.py.\n\n'
        'Upload (the user only): tools\\run_nexus_upload_hairwork.bat --version %s [--dry-run]\n'
        'Also needed by players of exported mods: Graphics Control (Nexus 13349), PT hair > Hair root transparency mode = Mode 0.\n'
        % (ver, a.aio_version, ver, K.CORE_VERSION, ver))
    print('release folder: %s' % os.path.relpath(out, ROOT))


if __name__ == '__main__':
    main()
