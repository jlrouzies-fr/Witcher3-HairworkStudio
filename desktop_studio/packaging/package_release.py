r"""package_release - build the Nexus release of Hairwork Studio (local only; publishes nothing).

  py desktop_studio\packaging\package_release.py [--version 0.1.0] [--sdk <HairWorks SDK folder>] [--no-runtime] [--no-zip]

Writes desktop_studio\dist\HairworkStudio-<version>\ (git-ignored) and dist\HairworkStudio-<version>.zip:
  HairworkStudio.exe, *.cso (our shaders, compiled), assets\ (our logo, OFL fonts + their licences)
  NvHairWorksDx11.win64.dll    <- the ONLY NVIDIA file, bundled with the app as the GameWorks EULA allows
  LICENSE.txt (Hairwork Studio License), core\LICENSE.md (Core licence), NVIDIA_HairWorks_EULA.txt, THIRD_PARTY_NOTICES.txt
  desktop_studio\mcp\{hairwork_studio_mcp,hws}.py = MCP server for agents; docs\agents\SKILL.md = agent skill
  README.txt, requirements.txt (only for running the tools from source)
  python\ (private embedded CPython + vendored pip packages, packaging\embedded_python.py; players install no Python)
  licenses\ (CPython PSF licence + every vendored package's licence files)
  The pipeline, bundled with the same relative paths the repo has (the exe finds it next to itself):
    desktop_studio\tools\{desk_prep,desk_export,calib}.py
    tools\{w3lib,cr2w_reader,cr2w_writer,texcache}.py
    tools\hairworks\<runtime modules>.py, APPROVED.json, roster.json,
      chars\<chosen base configs>.json, edits\<their edits>.json, lab\plans\{yen_material,yen_strands_only}.json
    core\hwcore.py      (Core version + release-export helper, no game data)
  build\desktop_studio\portraits_ai\*.jpg   our painted portraits (FLUX output)
  docs\ = desktop_studio\docs (user guide docs\guide\guide.html, opened by Help > User guide; agent guide) when present

Never copied: anything from the game (desk packs, meshes, textures, redfurs built from game data, captures, results\, build\),
third-party mods or their files, NVIDIA SDK headers / sources / docs / samples / Viewer, mods\wip, HAIKUSESSION*.
Users build their packs locally from their own game install (set the game folder in Settings).
The result is checked with  tools\sdk_guard.py --tree <folder> --release  (must pass).
"""
import argparse
import glob
import json
import os
import shutil
import subprocess
import sys
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import embedded_python
import agents_text

HERE = os.path.dirname(os.path.abspath(__file__))
DS = os.path.dirname(HERE)
ROOT = os.path.dirname(DS)
HW = os.path.join(ROOT, 'tools', 'hairworks')

ALLOWED_NVIDIA = {'NvHairWorksDx11.win64.dll'}

# Python modules the Studio's tools import or run (static + lazy imports of desk_prep / desk_export / calib, checked 2026-10-11
# with an import trace of real builds). Not bundled: the game-run harness (capture, winhdr, bg_capture, wgc16, hairmask, plot),
# yenref (reads the third-party Nexus 4587 mod), HairLab, ves_* experiments.
TOOLS_MODULES = ['w3lib.py', 'cr2w_reader.py', 'cr2w_writer.py', 'texcache.py']
HW_MODULES = ['approved.py', 'atlas.py', 'beard.py', 'collision.py', 'cr2w_edit.py', 'discover.py', 'flare.py', 'gamefiles.py',
              'gapfill.py', 'growth.py', 'hairpipe.py', 'headtex.py', 'hwpreview_pack.py', 'redfur.py', 'rig.py', 'scalp_tex.py',
              'silhouette.py', 'strand_edits.py', 'template.py', 'texread.py', 'validate_ent.py']
# tpl/fur02.redfur (the Yennefer mod's fur, legacy skeleton of the private dev repo) is NOT shipped: release builds take their
# skeleton at run time from the user's own game (redfur.py, mode 'game'). sdk_guard --tree also blocks it by name and SHA-1.
HW_DATA = ['APPROVED.json', 'roster.json',
           os.path.join('lab', 'plans', 'yen_material.json'), os.path.join('lab', 'plans', 'yen_strands_only.json')]
STUDIO_TOOLS = ['desk_prep.py', 'desk_export.py', 'calib.py']
# MCP server for coding agents (desktop_studio\mcp, same relative path as in the repo). autotune_* are internal: never shipped.
MCP_FILES = ['hairwork_studio_mcp.py', 'hws.py']
DOCS_IGNORE = shutil.ignore_patterns('__pycache__', 'feedback')   # feedback/ = private user reviews

# Base configs bundled: approved builds, every "start plain" base config (plain_configs.py), the material-preset sources of
# desk_prep.PRESETS, the reviewed colour picks (plain_configs.COLOUR_FROM). A config that names a third-party mod
# (source_mods / ref_fur) is left out: it cannot be built without that mod.
PRESET_CFGS = ['emhyr_l132_struct_m2', 'lambert_l76_ez10_vol5d_wr15_b07_f8', 'oriana_l141_a', 'triss_l82_fe_user1b',
               'avallach_l85b_hl70_wv_ln_hc']
THIRD_PARTY_KEYS = ('source_mods', 'ref_fur')

REQUIREMENTS = """# Hairwork Studio pipeline packages. NOT needed to use the release: the release bundles its own Python (python\\ folder) with these.
# Only for running the tools from source with your own Python 3.10+ (tested with 3.13):  py -m pip install -r requirements.txt
numpy>=2.0
Pillow>=10.0
scipy>=1.11
lz4>=4.0
"""

README = """Hairwork Studio %(v)s  (part of Hairwork for All, The Witcher 3 next-gen / 5.0)
by WinterElfeas   https://www.nexusmods.com/witcher3/mods/13996

WHAT IT IS
Hairwork Studio converts the game's hair cards of a character to real NVIDIA HairWorks strands, lets you edit and simulate them
(gravity, stiffness, wind, collisions...) and exports a Witcher 3 mod. The strand data is built from YOUR game install: this
package contains no game files.

INSTALL
1. Windows 10/11 x64 with a DirectX 11 GPU and The Witcher 3 (GOG / Steam / Epic, patch 5.0, DX12 version installed).
2. Unzip this package anywhere with write access (not Program Files) and start HairworkStudio.exe. Nothing else to install: the
   Python the pipeline needs is bundled (python\\ folder, a private copy; it does not touch any Python you may have).
3. The Studio looks for the game in the usual Steam / GOG / Epic folders. If it does not find it, open Settings and set
   "Game folder" to your The Witcher 3 folder (the one that contains content\\content0 and mods), then restart.
4. Pick a character. The first time, the Studio builds that character's strand pack from your game files (a few minutes for
   some characters). Edit, then use Export. It writes its work files into its own build\\ folder.

EXPORTING A MOD TO SHARE ("Export for release")
The release export makes a mod folder (and zip) that needs two other mods installed by everybody who uses it:
  - Hairwork for All - Core (Nexus 13996): https://www.nexusmods.com/witcher3/mods/13996
  - Graphics Control (Nexus 13349): https://www.nexusmods.com/witcher3/mods/13349 with
    PT hair > "Hair root transparency mode" = Mode 0 (path-traced hair shows see-through gaps in mode 1)
The exported mod's REQUIREMENTS.txt and INSTALL_INSTRUCTIONS.txt say so. Credit: made with Hairwork Studio (link above).
The plain "Export" (dev export) makes a mod that works without the Core, for your own testing. Nothing is ever written into
your game's mods folder by the Studio; copy the exported folder there yourself.

LICENCE
Hairwork Studio License (LICENSE.txt): free for personal use, shared hair mods must keep the Core requirement and credit, no
commercial use. Hairwork for All Core: core\\LICENSE.md. NVIDIA HairWorks runtime (NvHairWorksDx11.win64.dll) is under the
NVIDIA GameWorks EULA (NVIDIA_HairWorks_EULA.txt). Other notices: THIRD_PARTY_NOTICES.txt.
Uses NVIDIA HairWorks, NVIDIA GameWorks technology, (c) NVIDIA Corporation.

NOT INCLUDED
No CD Projekt game files, no third-party mods, no NVIDIA SDK. Characters that need a third-party hair mod to build (e.g. Triss
with the long-hair mod) are not in the list.

Bundled Python: CPython %(pyv)s (embeddable package from python.org, PSF licence) plus numpy, Pillow, scipy, lz4; their licences are in
the licenses\\ folder and THIRD_PARTY_NOTICES.txt.

Source code: https://github.com/jlrouzies-fr/Witcher3-HairworkStudio

"""


def read(p):
    return open(p, encoding='utf-8-sig').read()


def chosen_configs():
    """-> {cfg name: why}"""
    sel = {}
    A = json.load(open(os.path.join(HW, 'APPROVED.json'), encoding='utf-8'))
    for n in A.get('approved', {}).values():
        sel[n] = 'approved'
    for st in A.get('approved_styles', {}).values():
        if isinstance(st, dict):
            for n in st.values():
                sel[n] = 'approved style'
    R = json.load(open(os.path.join(HW, 'roster.json'), encoding='utf-8'))['characters']
    for ch, c in R.items():
        for hs in c.get('hairstyles', []):
            n = ch if hs['id'] == 'default' else ch + '_' + hs['id']
            if os.path.exists(os.path.join(HW, 'chars', n + '.json')):
                sel.setdefault(n, 'plain base')
    for n in PRESET_CFGS:
        sel.setdefault(n, 'material preset')
    sys.path.insert(0, HW)
    import ast
    t = ast.parse(read(os.path.join(HW, 'plain_configs.py')))
    for node in ast.walk(t):
        if isinstance(node, ast.Assign) and getattr(node.targets[0], 'id', '') == 'COLOUR_FROM':
            for v in node.value.values:
                sel.setdefault(v.value, 'colour pick')
    out, skipped = {}, []
    for n, why in sorted(sel.items()):
        p = os.path.join(HW, 'chars', n + '.json')
        if not os.path.exists(p):
            skipped.append((n, 'missing'))
            continue
        C = json.loads(read(p))
        bad = [k for k in THIRD_PARTY_KEYS if C.get(k)]
        if bad:
            skipped.append((n, 'needs a third-party mod (%s)' % ','.join(bad)))
            continue
        out[n] = (why, C)
    return out, skipped


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--version', default='0.1.0')
    ap.add_argument('--sdk', default=os.path.join(ROOT, 'sources', 'HairWorks-r1_2_1-15-distro', 'HairWorks'))
    ap.add_argument('--build', default=os.path.join(DS, 'build', 'Release'))
    ap.add_argument('--no-runtime', action='store_true', help='leave the NVIDIA DLL out (users point Settings at their own)')
    ap.add_argument('--no-zip', action='store_true')
    ap.add_argument('--draft', action='store_true', help='allow the unfilled EULA template (internal test packages)')
    a = ap.parse_args()
    eula = open(os.path.join(HERE, 'EULA_template.txt'), encoding='utf-8').read()
    if '[' in eula and not a.draft:
        raise SystemExit('fill in the [bracketed] fields of packaging\\EULA_template.txt first (or pass --draft)')
    exe = os.path.join(a.build, 'HairworkStudio.exe')
    if not os.path.exists(exe):
        raise SystemExit('build the app first (cmake --build desktop_studio\\build --config Release, or pass --build <dir>)')
    name = 'HairworkStudio-' + a.version
    dist = os.path.join(DS, 'dist')
    out = os.path.join(dist, name)
    if os.path.isdir(out):
        shutil.rmtree(out)
    os.makedirs(out)

    def put(src, rel=None):
        dst = os.path.join(out, rel or os.path.relpath(src, ROOT))
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copy2(src, dst)

    shutil.copy2(exe, out)
    for cso in glob.glob(os.path.join(a.build, '*.cso')):
        shutil.copy2(cso, out)
    shutil.copytree(os.path.join(a.build if os.path.isdir(os.path.join(a.build, 'assets')) else DS, 'assets'),
                    os.path.join(out, 'assets'), ignore=shutil.ignore_patterns('__pycache__'))
    # ---- pipeline
    for t in STUDIO_TOOLS:
        put(os.path.join(DS, 'tools', t))
    for t in TOOLS_MODULES:
        put(os.path.join(ROOT, 'tools', t))
    for t in HW_MODULES:
        put(os.path.join(HW, t))
    for t in HW_DATA:
        put(os.path.join(HW, t))
    for t in MCP_FILES:
        put(os.path.join(DS, 'mcp', t))
    put(os.path.join(ROOT, 'core', 'hwcore.py'))
    put(os.path.join(ROOT, 'core', 'LICENSE.md'))
    cfgs, skipped = chosen_configs()
    n_edits = 0
    for n, (why, C) in cfgs.items():
        put(os.path.join(HW, 'chars', n + '.json'))
        if C.get('edits'):
            src = os.path.join(HW, 'edits', C['edits'])
            if os.path.exists(src):
                put(src)
                n_edits += 1
    # ---- painted portraits (ours; user decision 2026-10-11)
    pa = os.path.join(ROOT, 'build', 'desktop_studio', 'portraits_ai')
    n_port = 0
    for p in sorted(glob.glob(os.path.join(pa, '*.jpg'))):
        put(p)
        n_port += 1
    # ---- docs (written by another task) when present
    docs = os.path.join(DS, 'docs')
    if os.path.isdir(docs):
        shutil.copytree(docs, os.path.join(out, 'docs'), ignore=DOCS_IGNORE)
    else:
        print('note: desktop_studio\\docs not found, the user guide is not in this package')
    if not a.no_runtime:
        dll = os.path.join(a.sdk, 'bin', 'win64', 'NvHairWorksDx11.win64.dll')
        if not os.path.exists(dll):
            raise SystemExit('runtime DLL not found: ' + dll)
        shutil.copy2(dll, out)
    shutil.copy2(os.path.join(DS, 'LICENSE.md'), os.path.join(out, 'LICENSE.txt'))   # Hairwork Studio License
    with open(os.path.join(out, 'NVIDIA_HairWorks_EULA.txt'), 'w', encoding='utf-8') as fh:
        fh.write(eula.replace('{VERSION}', a.version))
    notices = ['THIRD-PARTY NOTICES', '',
               'NVIDIA HairWorks runtime (NvHairWorksDx11.win64.dll): proprietary, NVIDIA GameWorks EULA, see NVIDIA_HairWorks_EULA.txt.',
               'Uses NVIDIA HairWorks, NVIDIA GameWorks technology, (c) NVIDIA Corporation.', '',
               'Dear ImGui (MIT) - https://github.com/ocornut/imgui', 'nlohmann/json (MIT) - https://github.com/nlohmann/json',
               'stb_image / stb_image_write (public domain or MIT) - https://github.com/nothings/stb',
               'Cinzel and Inter fonts (SIL Open Font License 1.1) - assets/fonts/OFL_*.txt',
               'Lucide icons (ISC) - assets/fonts/LICENSE_lucide.txt', '']
    for lic in ('imgui/LICENSE.txt', 'nlohmann/LICENSE.MIT'):
        notices += ['---- ' + lic, open(os.path.join(DS, 'third_party', lic), encoding='utf-8').read(), '']
    # ---- private embedded Python (players install no Python) + the licences of everything vendored in it
    py_dir = embedded_python.build(os.path.join(out, 'python'))
    lic_dir = os.path.join(out, 'licenses')
    for src, rel in embedded_python.licenses_for(py_dir):
        dst = os.path.join(lic_dir, rel)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copy2(src, dst)
    pkgs = sorted({r.split('/')[0] for _, r in embedded_python.licenses_for(py_dir) if r.split('/')[0] != 'python'})
    notices += ['Bundled Python (python\\ folder, used only by the Studio\'s pipeline scripts):',
                'CPython %s, embeddable package from python.org (PSF License) - licenses/python/LICENSE_CPython_PSF.txt and python/LICENSE.txt' % embedded_python.PY_VERSION,
                'Vendored pip packages: ' + ', '.join(pkgs) + ' (licences in licenses/<package>/; numpy / scipy list their bundled libraries',
                'such as OpenBLAS, LAPACK, GCC runtime and others in their licence files).', '']
    open(os.path.join(out, 'THIRD_PARTY_NOTICES.txt'), 'w', encoding='utf-8').write('\n'.join(notices))
    open(os.path.join(out, 'README.txt'), 'w', encoding='utf-8').write(README % dict(v=a.version, pyv=embedded_python.PY_VERSION) + agents_text.agents_txt())
    open(os.path.join(out, 'requirements.txt'), 'w', encoding='utf-8').write(REQUIREMENTS)
    # ---- guards
    for dp, dn, fns in os.walk(out):
        if dp == out and 'python' in dn:
            dn.remove('python')        # third-party Python runtime + packages (no NVIDIA material; sdk_guard still scans it below)
        for fn in fns:
            low = fn.lower()
            if (low.startswith('nv') or 'hairworks' in low or low.endswith(('.h', '.hlsl', '.apx', '.lib'))) and fn not in ALLOWED_NVIDIA \
                    and fn not in ('NVIDIA_HairWorks_EULA.txt', 'HairworkStudio.exe') and not low.endswith('.json') and not low.endswith('.py'):
                raise SystemExit('refusing to package ' + os.path.join(dp, fn))
            if dp.replace('\\', '/').split('/' + name)[-1].startswith(('/results', '/build/hair', '/mods')):
                raise SystemExit('refusing to package ' + os.path.join(dp, fn))
    r = subprocess.run([sys.executable, os.path.join(ROOT, 'tools', 'sdk_guard.py'), '--tree', out, '--release'])
    if r.returncode:
        raise SystemExit('sdk_guard --release blocked the release folder')
    files = [os.path.join(dp, f) for dp, _, fs in os.walk(out) for f in fs]
    print('release folder: %s  (%d files, %.1f MB)' % (os.path.relpath(out, ROOT), len(files), sum(os.path.getsize(f) for f in files) / 1e6))
    print('configs: %d bundled (%d edits files), %d portraits; left out: %s' % (len(cfgs), n_edits, n_port, skipped))
    if not a.no_zip:
        zp = os.path.join(dist, name + '.zip')
        if os.path.exists(zp):
            os.remove(zp)
        with zipfile.ZipFile(zp, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
            for f in sorted(files):
                z.write(f, os.path.join(name, os.path.relpath(f, out)))
        print('zip: %s (%.1f MB)  (not published)' % (zp, os.path.getsize(zp) / 1e6))


if __name__ == '__main__':
    main()
