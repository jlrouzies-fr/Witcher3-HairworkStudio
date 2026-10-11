r"""export_public - make the public GitHub repo of Hairwork Studio as a FRESH folder (no private history). Local only.

  py tools\export_public.py [--out <folder>] [--clean]

Default folder: <game>\Witcher3-HairworkStudio-public (outside this repo). Copies exactly: desktop_studio sources (no build / dist /
design / ai_* tools), the bundled pipeline (same file lists as desktop_studio\packaging\package_release.py), core sources,
licences, the sdk_guard hook / CI, the painted portraits and the docs. Then `git init`, one commit, core.hooksPath .githooks,
and tools\sdk_guard.py --tree must pass. No remote is added and nothing is pushed.
"""
import argparse
import glob
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
GAME = os.path.dirname(ROOT)
sys.path.insert(0, os.path.join(ROOT, 'desktop_studio', 'packaging'))
import package_release as PR  # noqa: E402

DS_SKIP_TOOLS = {'ai_logo_upscale.py', 'ai_portrait_test.py', 'make_portraits.py', 'lookcompare.py', '__pycache__'}
IGN = shutil.ignore_patterns('__pycache__', '*.pyc')

README = """# Hairwork Studio

Hairwork Studio is a native Windows editor and simulator for **NVIDIA HairWorks** hair in **The Witcher 3** (next-gen patch 5.0, DX12).
It converts a character's vanilla hair cards to real HairWorks strands built from *your own* game install, lets you edit and
simulate them with the real HairWorks 1.2.1 runtime (gravity, stiffness, damping, wind, collisions, density map, LOD) and exports a
Witcher 3 mod. Part of **Hairwork for All**.

- Nexus Mods (downloads, the Core mod): https://www.nexusmods.com/witcher3/mods/13996
- User guide: [`desktop_studio/docs/guide/guide.html`](desktop_studio/docs/guide/guide.html), agent / scripting guide: [`desktop_studio/docs/AGENT_GUIDE.md`](desktop_studio/docs/AGENT_GUIDE.md)

## Licences (which covers what)

| Part | Licence |
|---|---|
| `desktop_studio/` (the Studio: source, shaders, tools, assets made for it) | **Hairwork Studio License**, source-available, not open source: [`desktop_studio/LICENSE.md`](desktop_studio/LICENSE.md). Personal use and modification are free; shared hair mods must keep the Core requirement and credit; no commercial use. |
| `core/` (Hairwork for All - Core, the dependency mod) | **Core license**: [`core/LICENSE.md`](core/LICENSE.md) |
| everything else (`tools/`, the pipeline) | MIT, [`LICENSE.md`](LICENSE.md) |
| Third-party libraries in `desktop_studio/third_party/` and fonts / icons in `desktop_studio/assets/fonts/` | their own licences (Dear ImGui MIT, nlohmann/json MIT, stb public domain / MIT, Cinzel + Inter SIL OFL 1.1, Lucide ISC) |
| **NVIDIA HairWorks** | proprietary, NVIDIA GameWorks EULA. **Not in this repository.** |

## What is NOT here

- **The NVIDIA HairWorks SDK** (headers, sources, docs, samples, Viewer, DLLs). Its source, headers and documentation are NVIDIA
  Confidential Information under the GameWorks EULA, so they must never be committed. A pre-commit hook and a CI job
  (`tools/sdk_guard.py`) block them. Only the Nexus *release* ships the runtime DLL `NvHairWorksDx11.win64.dll`, as the EULA allows.
- Any file from the game, third-party mods, captures or results. The pipeline reads the game's files from your install at run time.

## Build the Studio

Needs Windows 10/11 x64, Visual Studio 2022 Build Tools (Desktop development with C++, Windows SDK 10.0.26100 or newer) and CMake 3.20+.
You also need the **NVIDIA HairWorks 1.2.1 SDK** (`HairWorks-r1_2_1-15-distro`) from NVIDIA's GameWorks download page: accept their EULA
and unzip it anywhere; it is only read at build time (headers, shader header) and run time (runtime DLL).

```
cmake -S desktop_studio -B desktop_studio/build -G "Visual Studio 17 2022" -A x64 -DHAIRWORKS_SDK_DIR=<...>\\HairWorks
cmake --build desktop_studio/build --config Release
```

`HAIRWORKS_SDK_DIR` must contain `include/Nv/HairWorks/NvHairSdk.h` and `bin/win64/NvHairWorksDx11.win64.dll`. The exe, compiled
shaders and assets land in `desktop_studio/build/Release/`. Details: [`desktop_studio/README.md`](desktop_studio/README.md).

## Run

Building from source needs Python 3.10+ (tested 3.13) with the pipeline requirements: `py -m pip install -r requirements.txt`.
The Nexus release does not: `desktop_studio/packaging/package_release.py` bundles a private embedded Python (the python.org "embeddable package" +
the pinned pip packages, see `desktop_studio/packaging/embedded_python.py`; downloaded and cached under `build/python_embed/` at package time,
not stored in this repository) as `python/` next to the exe, so players install no Python.
Start `desktop_studio/build/Release/HairworkStudio.exe`. It finds the pipeline (`tools/`) by walking up from the exe. Set your
Witcher 3 folder in Settings > "Game folder" (or the environment variable `HWFA_GAME`) if it is not found automatically.
The first time you open a character the Studio builds its strand pack from your game files.

## Release export and mod requirements

"Export for release" makes a mod that needs, for everybody who plays it:

- **Hairwork for All - Core** (Nexus 13996): https://www.nexusmods.com/witcher3/mods/13996 (source in `core/`)
- **Graphics Control** with PT hair > "Hair root transparency mode" = Mode 0: https://www.nexusmods.com/witcher3/mods/13349

## Contributing

Pull requests are welcome under the Hairwork Studio License (see section 1.3 of the licence). Run
`git config core.hooksPath .githooks` once per clone so the SDK guard runs before every commit; never add NVIDIA SDK files.
"""

GITIGNORE = """__pycache__/
*.pyc
# build outputs, work folders written by the tools
build/*
!build/desktop_studio/
build/desktop_studio/*
!build/desktop_studio/portraits_ai/
dist/
release/
results/
sources/
"""


def cp(src, dst):
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    shutil.copy2(src, dst)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', default=os.path.join(GAME, 'Witcher3-HairworkStudio-public'))
    ap.add_argument('--clean', action='store_true', help='replace an existing export folder')
    a = ap.parse_args()
    out = os.path.abspath(a.out)
    if os.path.commonpath([out, ROOT]) == ROOT:
        sys.exit('the export must be outside the private repo')
    if os.path.exists(out):
        if not a.clean:
            sys.exit('%s exists (use --clean to replace it)' % out)
        subprocess.run(['cmd', '/c', 'rmdir', '/s', '/q', out], check=False)
    os.makedirs(out)
    ds = os.path.join(ROOT, 'desktop_studio')
    for d in ('src', 'shaders', 'assets', 'third_party', 'packaging', 'docs'):
        if os.path.isdir(os.path.join(ds, d)):
            shutil.copytree(os.path.join(ds, d), os.path.join(out, 'desktop_studio', d), ignore=IGN)
    os.makedirs(os.path.join(out, 'desktop_studio', 'tools'))
    for f in os.listdir(os.path.join(ds, 'tools')):
        if f not in DS_SKIP_TOOLS:
            cp(os.path.join(ds, 'tools', f), os.path.join(out, 'desktop_studio', 'tools', f))
    shutil.copytree(os.path.join(ds, 'tests'), os.path.join(out, 'desktop_studio', 'tests'), ignore=IGN)
    for f in ('CMakeLists.txt', 'README.md', 'LICENSE.md', '.gitignore'):
        cp(os.path.join(ds, f), os.path.join(out, 'desktop_studio', f))
    # bundled pipeline (same lists as the Nexus release)
    for t in PR.TOOLS_MODULES:
        cp(os.path.join(ROOT, 'tools', t), os.path.join(out, 'tools', t))
    for t in PR.HW_MODULES + PR.HW_DATA:
        cp(os.path.join(PR.HW, t), os.path.join(out, 'tools', 'hairworks', t))
    cfgs, _skipped = PR.chosen_configs()
    for n, (_why, C) in cfgs.items():
        cp(os.path.join(PR.HW, 'chars', n + '.json'), os.path.join(out, 'tools', 'hairworks', 'chars', n + '.json'))
        if C.get('edits') and os.path.exists(os.path.join(PR.HW, 'edits', C['edits'])):
            cp(os.path.join(PR.HW, 'edits', C['edits']), os.path.join(out, 'tools', 'hairworks', 'edits', C['edits']))
    # tools for the repo itself: guard, release + export scripts
    for t in ('sdk_guard.py', 'build_release_hairwork.py', 'build_allinone.py', 'export_public.py'):
        cp(os.path.join(HERE, t), os.path.join(out, 'tools', t))
    # core sources
    for f in os.listdir(os.path.join(ROOT, 'core')):
        p = os.path.join(ROOT, 'core', f)
        if os.path.isfile(p):
            cp(p, os.path.join(out, 'core', f))
    # guard hook + CI
    cp(os.path.join(ROOT, '.githooks', 'pre-commit'), os.path.join(out, '.githooks', 'pre-commit'))
    cp(os.path.join(ROOT, '.github', 'workflows', 'sdk-guard.yml'), os.path.join(out, '.github', 'workflows', 'sdk-guard.yml'))
    cp(os.path.join(ROOT, 'requirements.txt'), os.path.join(out, 'requirements.txt'))
    open(os.path.join(out, 'requirements.txt'), 'w').write(PR.REQUIREMENTS)
    # portraits (ours, user decision 2026-10-11)
    for p in sorted(glob.glob(os.path.join(ROOT, 'build', 'desktop_studio', 'portraits_ai', '*.jpg'))):
        cp(p, os.path.join(out, 'build', 'desktop_studio', 'portraits_ai', os.path.basename(p)))
    # licences / README
    lic = open(os.path.join(ROOT, 'LICENSE.md'), encoding='utf-8').read()
    open(os.path.join(out, 'LICENSE.md'), 'w', encoding='utf-8').write(lic)
    open(os.path.join(out, 'README.md'), 'w', encoding='utf-8').write(README)
    open(os.path.join(out, '.gitignore'), 'w').write(GITIGNORE)
    # check, then ONE commit (no remote)
    r = subprocess.run([sys.executable, os.path.join(out, 'tools', 'sdk_guard.py'), '--tree', out])
    if r.returncode:
        sys.exit('sdk_guard blocked the export')
    g = lambda *x: subprocess.run(['git', '-C', out] + list(x), check=True)
    g('init', '-q', '-b', 'main')
    g('config', 'core.hooksPath', '.githooks')
    # the owner's private e-mail must not end up in public history: GitHub noreply identity of the account
    g('config', 'user.name', 'jlrouzies-fr')
    g('config', 'user.email', 'jlrouzies-fr@users.noreply.github.com')
    g('add', '-A')
    g('commit', '-q', '-m', 'Hairwork Studio 0.1.0: initial public export\n\nCo-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>')
    files = subprocess.run(['git', '-C', out, 'ls-files'], capture_output=True, text=True).stdout.split('\n')
    print('exported %d files to %s (one commit, no remote)' % (len([f for f in files if f]), out))


if __name__ == '__main__':
    main()
