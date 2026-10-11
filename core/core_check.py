r"""core_check - prove the Core's script dependency with the game's compile oracle (witcher3.exe -verifyscriptsandexit).

  py core\core_check.py [--core] <release mod folder> [<release mod folder> ...]

Installs the built Core (--core, core\build_core.py install) and copies each release mod folder into <game>\mods under
its own folder name (must not exist there yet: use a TEST name, e.g. hairpipe.py <cfg> build --release --mod
modXHairworksCoreTest), runs core\compile_oracle.ps1, prints the result as JSON ({"ok", "exit_code", "errors"}), and
removes everything it installed again (also on failure). Refuses while any witcher3.exe runs. A clean compile exits
with code 3; script errors open the "Script Compilation Errors" dialog, whose text is returned in "errors".
Only the scripts matter to the oracle; bundle paths shared with an installed mod are harmless for this check.
"""
import json
import os
import re
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
GAME = os.path.dirname(ROOT)
sys.path.insert(0, HERE)
import build_core  # noqa: E402
import hwcore as K  # noqa: E402


def oracle():
    out = subprocess.run(['powershell', '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File',
                          os.path.join(HERE, 'compile_oracle.ps1'),
                          '-Exe', os.path.join(GAME, 'bin', 'x64_dx12', 'witcher3.exe')],
                         capture_output=True, text=True, timeout=420).stdout
    m = re.search(r'exit code: (-?\d+)', out)
    code = int(m.group(1)) if m else None
    errs = None
    if '---- compilation dialog text ----' in out:
        text = out.split('---- compilation dialog text ----', 1)[1].split('---- end ----', 1)[0]
        errs = '\n'.join(ln.strip() for ln in text.splitlines() if ln.strip().startswith('Error')) or text.strip()
    # the dialog also lists hundreds of vanilla warnings: keep only the harness lines in the log
    out = '\n'.join(ln for ln in out.splitlines() if not ln.startswith('Warning '))
    return code, errs, out


def main(a):
    core = '--core' in a
    mods = [os.path.abspath(x) for x in a if x != '--core']
    if build_core.running():
        raise SystemExit('witcher3.exe is running: not testing (wait for it to exit)')
    have, ver = K.installed()
    if have and core:
        raise SystemExit('a Core (version %s) is already installed; not replacing it for a test' % ver)
    if have and not core:
        raise SystemExit('a Core (version %s) is installed; the no-Core test needs it absent' % ver)
    added = []
    try:
        if core:
            build_core.install()
            added.append(os.path.join(GAME, 'mods', K.CORE_MOD))
        for m in mods:
            dst = os.path.join(GAME, 'mods', os.path.basename(m))
            if os.path.exists(dst):
                raise SystemExit('%s already exists in <game>\\mods: use a test mod name' % os.path.basename(m))
            shutil.copytree(m, dst)
            added.append(dst)
            print('installed (test) %s' % dst)
        code, errs, raw = oracle()
    finally:
        for d in added:
            shutil.rmtree(d, ignore_errors=True)
            print('removed %s' % d)
    res = {'core': core, 'mods': [os.path.basename(m) for m in mods], 'exit_code': code,
           'ok': code == 3 and errs is None, 'errors': errs}
    print(raw.strip())
    print(json.dumps(res, indent=1))
    return res


if __name__ == '__main__':
    main(sys.argv[1:])
