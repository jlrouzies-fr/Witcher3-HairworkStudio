r"""build_core - build / verify / install the "Hairwork for All - Core" dependency mod (core/README.md).

  py core\build_core.py build        -> build\core\mod0000HairworkForAllCore\{version.json, LICENSE.md,
                                        content\scripts\local\hairworkforall_core.ws} + build\core\HairworkForAllCore_v<ver>.zip
  py core\build_core.py verify       re-read the built Core: script = hwcore.core_script(), version.json, no game files
  py core\build_core.py install      copy the build to <game>\mods\mod0000HairworkForAllCore (refuses while witcher3.exe runs)
  py core\build_core.py uninstall    remove <game>\mods\mod0000HairworkForAllCore (refuses while witcher3.exe runs)
  py core\build_core.py status       installed? which version? (JSON line)

v1.0.0 (2026-10-11): a script-only mod. No bundle: the game compiles mods\<mod>\content\scripts\**.ws without a
blob0.bundle / metadata.store (checked with witcher3.exe -verifyscriptsandexit and an in-game HairLab round, HAIRWORK.md
"Hairwork for All - Core v1"). It ships only our own files: the script, version.json and the licence.
"""
import json
import os
import shutil
import subprocess
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
GAME = os.path.dirname(ROOT)
sys.path.insert(0, HERE)
import hwcore as K  # noqa: E402

OUT = os.path.join(ROOT, 'build', 'core')
MOD_DIR = os.path.join(OUT, K.CORE_MOD)


def version_bytes():
    return (json.dumps(K.version_doc(), indent=2) + '\n').encode('utf-8')


def build():
    if os.path.isdir(MOD_DIR):
        shutil.rmtree(MOD_DIR)
    sp = os.path.join(MOD_DIR, K.SCRIPT_REL)
    os.makedirs(os.path.dirname(sp))
    with open(sp, 'wb') as fh:
        fh.write(K.core_script())
    with open(os.path.join(MOD_DIR, K.MARKER), 'wb') as fh:
        fh.write(version_bytes())
    shutil.copyfile(os.path.join(HERE, 'LICENSE.md'), os.path.join(MOD_DIR, 'LICENSE.md'))
    zp = os.path.join(OUT, 'HairworkForAllCore_v%s.zip' % K.CORE_VERSION)
    with zipfile.ZipFile(zp, 'w', zipfile.ZIP_DEFLATED) as z:
        for dp, _, fns in sorted(os.walk(MOD_DIR)):
            for fn in sorted(fns):
                full = os.path.join(dp, fn)
                z.write(full, os.path.relpath(full, OUT))
    files = []
    for dp, _, fns in os.walk(MOD_DIR):
        for fn in fns:
            full = os.path.join(dp, fn)
            files.append('%s %d B' % (os.path.relpath(full, MOD_DIR), os.path.getsize(full)))
    print('built %s v%s: %s; zip %s (%d B)' % (os.path.relpath(MOD_DIR, ROOT), K.CORE_VERSION, ', '.join(sorted(files)),
                                               os.path.relpath(zp, ROOT), os.path.getsize(zp)))
    return MOD_DIR


def verify(d=None):
    d = d or MOD_DIR
    ok = True
    sp = os.path.join(d, K.SCRIPT_REL)
    s_ok = os.path.isfile(sp) and open(sp, 'rb').read() == K.core_script()
    print('script %s: %s' % (K.SCRIPT_REL, 'ok' if s_ok else 'MISSING / DIFFERS'))
    try:
        vd = json.load(open(os.path.join(d, K.MARKER), encoding='utf-8'))
    except Exception:
        vd = {}
    v_ok = vd.get('version') == K.CORE_VERSION
    print('%s: %s' % (K.MARKER, vd.get('version')))
    extra = []
    for dp, _, fns in os.walk(d):
        for fn in fns:
            rel = os.path.relpath(os.path.join(dp, fn), d)
            if rel not in (K.SCRIPT_REL, K.MARKER, 'LICENSE.md'):
                extra.append(rel)
    if extra:
        print('unexpected files (the Core ships only our own script / version / licence): %s' % extra)
    ok = s_ok and v_ok and not extra
    print('VERIFY OK' if ok else 'VERIFY FAILED')
    return ok


def running():
    out = subprocess.run(['tasklist'], capture_output=True, text=True).stdout.lower()
    return 'witcher3.exe' in out


def install():
    if running():
        raise SystemExit('witcher3.exe is running: not installing the Core')
    if not os.path.isdir(MOD_DIR):
        raise SystemExit('no Core build: run "py core\\build_core.py build" first')
    dst = os.path.join(GAME, 'mods', K.CORE_MOD)
    # script files are merged by path across mods: another mod shipping the same .ws path would hide ours
    mine = K.SCRIPT_REL.lower()
    for m in os.listdir(os.path.join(GAME, 'mods')):
        p = os.path.join(GAME, 'mods', m)
        if os.path.normcase(p) != os.path.normcase(dst) and os.path.isfile(os.path.join(p, mine)):
            raise SystemExit('conflict: %s also ships %s' % (m, K.SCRIPT_REL))
    if os.path.isdir(dst):
        shutil.rmtree(dst)
    shutil.copytree(MOD_DIR, dst)
    print('installed Core v%s -> %s' % (K.CORE_VERSION, dst))
    return dst


def uninstall():
    if running():
        raise SystemExit('witcher3.exe is running: not removing the Core')
    dst = os.path.join(GAME, 'mods', K.CORE_MOD)
    shutil.rmtree(dst, ignore_errors=True)
    print('removed %s' % dst)


def main(a):
    cmd = a[0] if a else 'build'
    if cmd == 'build':
        build()
        if not verify():
            raise SystemExit(1)
    elif cmd == 'verify':
        if not verify(a[1] if len(a) > 1 else None):
            raise SystemExit(1)
    elif cmd == 'install':
        install()
    elif cmd == 'uninstall':
        uninstall()
    elif cmd == 'status':
        inst, ver = K.installed()
        print(json.dumps({'installed': inst, 'version': ver, 'required': K.CORE_VERSION, 'mod': K.CORE_MOD}))
    else:
        raise SystemExit(__doc__)


if __name__ == '__main__':
    main(sys.argv[1:])
