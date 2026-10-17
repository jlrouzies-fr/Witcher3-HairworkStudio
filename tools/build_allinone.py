r"""build_allinone - the main mod "Hairwork for All - All In One": RELEASE builds of the shipped characters. Local only.

  py tools\build_allinone.py --out <folder> [--aio-version 0.1.0] [--jobs 3]     (build_release_hairwork.py calls it)

For every character in AIO_ORDER: `hairpipe.py <cfg> build --release` (Core requirement script + REQUIREMENTS.txt per mod) with
HWFA_SKELETON=game (the CR2W skeleton of every redfur is read from the user's own game bundles; tpl\fur02.redfur, the Yennefer
mod's fur, is third-party and never used) and HWFA_MOD_OUT=build\aio\<v>\pkg (the repo's mods\ folders are not touched).
Layout of the zip = the installed layout: mod<Char>Hairworks\content\... at the zip root, README.txt, REQUIREMENTS.txt.
The Vesemir / Triss scalp head textures (from <game>\mods\mod<Char>Scalp) are MERGED into that character's hair mod: its
blob0.bundle gets the scalp xbm, metadata.store is regenerated with the repo's w3lib writers over all bundles, and the scalp's
texture.cache is copied next to it (merge_scalp). No separate scalp folder.
Checks: every mod has bundle + store + requirement script, no redfur carries importFile / the third-party marker, no
third-party file names, tools\sdk_guard.py --tree on the package.
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
import zipfile
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

# the characters it ships; APPROVED.json first, AIO_PICKS for those that are "settled / user OK" but not listed there
# (characters.md 2026-10-06). Emhyr (user: DONE 2026-10-12) = his Groom Studio project big_c/emhyr_mcp_8 as a pipeline
# config (opt-in studio_geo key: tools/hairworks/studio_geo.py)
AIO_ORDER = ['vesemir', 'c04ma', 'avallach', 'sheala', 'dandelion', 'keira', 'lambert', 'triss', 'guillaume', 'regis', 'emhyr', 'ciri', 'zoltan', 'yennefer', 'baron', 'dettlaff', 'hjalmar', 'ves', 'dijkstra', 'radovid', 'menge', 'palmerin', 'horst', 'graden', 'annastrenger', 'ewald', 'geels', 'marlene', 'shani']   # yennefer back in 2026-10-14 (user: "yes in all in one") with L316 (20.15 ms GPU)
AIO_PICKS = {'avallach': 'avallach_l85b_hl70_wv_ln_hc', 'sheala': 'sheala_l89_rec_sk6_sink5only',
             'keira': 'keira_l108_user1_c09', 'lambert': 'lambert_l76_ez10_vol5d_wr15_b07_f8',
             'emhyr': 'emhyr_l255_mcp8',
             'ciri': 'ciri_l259_cr10rib',
             'yennefer': 'yennefer_l365_user',  # 2026-10-15 user "for release and game run": Studio yen_split/yennefer_user_2025 (split groom + user edits); was yennefer_l330_user. 2026-10-15 user "my latest Yenn save: go for release" (vesgen/yennefer_user_1112, density 1.3->1.11 for budget, perf accepted by user). Was yennefer_l316_mcp35s, 2026-10-14: Studio yen6/yennefer_mcp_35 (top front fix) with the user's stiffness floor (0.717 / root 0.366 / tip 0), L316 20.15 ms GPU
             'zoltan': 'zoltan_l302_perf',   # user 'he is good' 2026-10-14: Studio zol1/zoltan_mcp_4 beard (studio_geo_beard); L302 GPU -2.2 ms vs Vesemir
             'baron': 'baron_l323_user',
             'dettlaff': 'dettlaff_l352_user',   # 2026-10-15 user: Studio edit of dettlaff_l350_opt (k10 + Coarse short, width .675) for release; was dettlaff_l326_user
             'hjalmar': 'hjalmar_l327_user', 'ves': 'ves_l331_user',
             # 2026-10-15 user "i edited: radovid, menge, djikstra; rest is good, all 5 ready for release and next game run": Studio beard-only look / physics edits (studio_geo_beard) for Radovid, Menge, Dijkstra (beard_<char>/<char>_user_1302); Palmerin, Horst as built
             'dijkstra': 'dijkstra_l336_user', 'radovid': 'radovid_l336_user', 'menge': 'menge_l336_user',
             'palmerin': 'palmerin_l332_beard', 'horst': 'horst_l334_beard',
             'graden': 'graden_l357_user',
             'annastrenger': 'annastrenger_l361_user',
             'ewald': 'ewald_l367_user',
             'geels': 'geels_l368_user',
             'marlene': 'marlene_l561_mcp17'}   # 2026-10-16 user mcp14 approved + 'a look change on that area is OK': left-temple root stubs fixed (folded growth faces re-triangulated, right low-temple faces removed; guides / colliders / look = mcp14), L561   # 2026-10-16 user "fix only Marlene's temple tufts ... then add her to the All In One if clean" + "fix the flare": her 18:11 Studio save (marlene_user_1811, L489) with the temple tufts re-laid and the ear spheres refit (marlene_t1/marlene_mcp_14); prepared, not built   # 2026-10-15 user "ready for release and gpu run": Studio hair_geels/geels_user_2116   # 2026-10-15 user "put him for release and next game run": Studio hair_ewald/ewald_user_2101, L367 18.9 ms GPU   # 2026-10-15 user "she is candidate for release and GPU run": Studio hair_annastrenger/annastrenger_user_2010   # 2026-10-15 user "ready for release and game run": Studio hair_graden5/graden_user_1919 (--allow-perf, L357 25.2 ms GPU)   # 2026-10-15 user "go for my last Ves edit": Studio vesgen/ves_round_37 user save (ves_user_1126_r37), GPU cost 239% accepted by the user   # 2026-10-15 user "hjal is good, ready for release": hair+beard Studio edit, hair colour = beard   # 2026-10-15 user "install my last dettlaff edit for the next release": Studio dett8/dettlaff_user_0905, exported --allow-perf (user: "I want my dettlaff edit")   # 2026-10-15 user "my barron edit btw > good for release in all in one": Studio baron1/baron_user_0646 beard-only (studio_geo_beard, Coarse short)
# path-tracing picks (user OK 2026-10-13, measured with fpsrun): these win over APPROVED.json, which stays frozen
AIO_PT_PICKS = {'lambert': 'lambert_l285_me3', 'guillaume': 'guillaume_l275_p09r15c10', 'triss': 'triss_l275_c07',
                'regis': 'regis_l323_user',
                'shani': 'shani_l627_eyefix'}  # 2026-10-17 user 'Shani with eye fix can go in all in one': her Studio save shani_user_1427 minus 2 guides whose interpolated hair crossed her left eye (shani_eye1/shani_mcp_17); not shot in game yet (L627 pending), GPU not measured. Regis: 2026-10-15: user's own Studio edit of Regis (regis2/regis_user_0641) + Coarse short HairWorks beard, replaces regis_v14 ("put that in next release"); here because APPROVED.json wins over AIO_PICKS
# folder name overrides: Triss loads before the lore-friendly Triss mod (modtriss_lorehair), which otherwise overrides her hair
AIO_MOD_NAMES = {'triss': 'mod000_TrissHairworks'}
# optional folders (zip folder optional\<mod>, so "copy everything" does not install them): built with --release --mod-guard
# --mod <mod>. Headband Ciri (user: add to the AIO 2026-10-14) converts bububull's "Ciri - long wavy hair" (Nexus 7063) and
# needs it at runtime (mod_guard.py); it patches the same ciri.w2ent as modCiriHairworks, so users install one of the two
AIO_OPTIONAL = {'ciri_headband': ('ciri_l313_guard', 'mod000_CiriWavyHairworks')}
OPTIONAL_DIR = 'optional'
AIO_EXTRA_MODS = {'vesemir': 'modVesemirScalp', 'triss': 'modTrissScalp'}
AIO_NAMES = {'vesemir': 'Vesemir', 'c04ma': 'generic short-hair men (c04ma, 79 NPCs)', 'avallach': "Avallac'h", 'sheala': 'Sheala',
             'dandelion': 'Dandelion', 'keira': 'Keira Metz', 'lambert': 'Lambert', 'triss': 'Triss (needs the lore-friendly Triss mod)',
             'guillaume': 'Guillaume', 'regis': 'Regis', 'emhyr': 'Emhyr var Emreis', 'ciri': 'Ciri',
             'yennefer': 'Yennefer (travel outfit)', 'zoltan': 'Zoltan Chivay', 'baron': 'Bloody Baron (beard)', 'dettlaff': 'Dettlaff', 'hjalmar': 'Hjalmar', 'ves': 'Ves',
             'dijkstra': 'Sigismund Dijkstra (beard)', 'radovid': 'Radovid V (beard)', 'menge': 'Caleb Menge (beard)',
             'palmerin': 'Palmerin de Launfal (beard)', 'horst': 'Horst Borsodi (beard)', 'graden': 'Graden', 'annastrenger': 'Anna Strenger', 'ewald': 'Ewald', 'geels': "Ge'els", 'marlene': 'Marlene', 'shani': 'Shani',
             'ciri_headband': 'Ciri with headband (optional folder, needs "Ciri - long wavy hair" by bububull)'}

README = """Hairwork for All - All In One %(v)s  (Nexus witcher3 mod 13996; patch 5.0 next-gen, DX12)
by WinterElfeas

NVIDIA HairWorks strands instead of hair cards for:
%(chars)s

REQUIRED (the game will not start without the first one: every mod here carries a script that checks for it)
  1. Hairwork for All - Core, v%(core)s or newer: the file "Hairwork for All - Core" of this same Nexus page
     (https://www.nexusmods.com/witcher3/mods/13996). Copy mod0000HairworkForAllCore into <game>\\mods.
  2. Graphics Control (https://www.nexusmods.com/witcher3/mods/13349): PT hair > "Hair root transparency mode" = Mode 0
     (path-traced hair shows see-through gaps in mode 1).
  3. HairWorks enabled in the game's graphics options.

INSTALL
  Copy the mod folders you want from this zip into <game>\\mods (Core first). Remove a folder to get that character's vanilla
  hair back. One folder per character: modVesemirHairworks, modC04maHairworks, ... The Vesemir and mod000_TrissHairworks folders also hold
  the recoloured scalp texture under the hair (the game's own head texture, recoloured, and nothing else).

TRISS
  Triss's hair is built on the "lore-friendly long hair for Triss" mod (Nexus 6457) by its author. Install that mod yourself:
  it is NOT included here, and this one needs it. A small script in her folder checks for it while the game runs: without
  that mod, Triss keeps her vanilla hair (no HairWorks) and the game shows a message that asks for it.
  Her folder is named mod000_TrissHairworks on purpose: the name sorts before modtriss_lorehair, so this mod loads first and
  the lore-friendly hair mod does not override her HairWorks hair. Do not rename it.
  If you use a mods.settings priority list (Documents\\The Witcher 3\\mods.settings, written by Script Merger or Vortex),
  give mod000_TrissHairworks a higher priority than modtriss_lorehair there: that list overrides the alphabetical order.

CIRI WITH HEADBAND (optional)
  The folder optional\\mod000_CiriWavyHairworks is Ciri's long wavy hair with headband from the mod "Ciri - long wavy hair"
  by bububull (Nexus 7063), converted to HairWorks. Install that mod yourself: it is NOT included here, and this folder needs
  it. A small script in the folder checks for it while the game runs: without it, Ciri keeps her vanilla hair and the game
  shows a message that asks for it.
  Install ONE Ciri folder: modCiriHairworks (her vanilla haircut) OR mod000_CiriWavyHairworks (headband). Both change the
  same Ciri template; with both installed, the headband one wins. Copy the mod000_CiriWavyHairworks folder itself into
  <game>\\mods, not the optional folder.

Mods that replace the same hair templates of these characters conflict (the later mod folder wins).
Made with Hairwork Studio (same Nexus page). Uses NVIDIA HairWorks, NVIDIA GameWorks technology, (c) NVIDIA Corporation.
No third-party mod files are included.
"""


def configs():
    """-> [(char, cfg, mod)] in AIO_ORDER."""
    sys.path.insert(0, os.path.join(ROOT, 'tools', 'hairworks'))
    import approved
    dm = approved.default_map()
    out = []
    for ch in AIO_ORDER:
        cfg = AIO_PT_PICKS.get(ch) or dm.get(ch) or AIO_PICKS.get(ch)
        if not cfg:
            sys.exit('no config for ' + ch)
        if ch in AIO_PICKS and ch not in AIO_PT_PICKS and dm.get(ch) not in (None, cfg):
            print('note: APPROVED.json now has %s for %s (AIO_PICKS: %s): using APPROVED' % (dm[ch], ch, AIO_PICKS[ch]))
        with open(os.path.join(ROOT, 'tools', 'hairworks', 'chars', cfg + '.json'), encoding='utf-8-sig') as fh:
            C = json.load(fh)
        out.append((ch, cfg, AIO_MOD_NAMES.get(ch, C['mod'])))
    return out


def merge_scalp(hair_dir, scalp_dir):
    """Merge a scalp mod (blob0.bundle + texture.cache) into a hair mod: same files, same bytes per entry. The bundles and
    metadata.store are rewritten with w3lib's writers (no hand-swapped metadata.store); texture.cache is copied."""
    sys.path.insert(0, os.path.join(ROOT, 'tools'))
    import w3lib
    hc, sc = os.path.join(hair_dir, 'content'), os.path.join(scalp_dir, 'content')
    groups = []
    for bn in ('blob0.bundle', 'buffers0.bundle'):
        hp = os.path.join(hc, bn)
        groups.append([(e.name, w3lib.bundle_read(hp, e)) for e in w3lib.bundle_entries(hp)] if os.path.isfile(hp) else [])
    sp = os.path.join(sc, 'blob0.bundle')
    extra = [(e.name, w3lib.bundle_read(sp, e)) for e in w3lib.bundle_entries(sp)]
    have = {n.lower() for g in groups for n, _ in g}
    dup = [n for n, _ in extra if n.lower() in have]
    if dup:
        sys.exit('scalp merge: file names in both mods: %s' % dup)
    if os.path.isfile(os.path.join(hc, 'texture.cache')) or not os.path.isfile(os.path.join(sc, 'texture.cache')):
        sys.exit('scalp merge: unexpected texture.cache state in %s / %s' % (hair_dir, scalp_dir))
    groups[0] += extra
    bundles = [w3lib.write_legacy_bundle(os.path.join(hc, 'blob0.bundle'), groups[0])]
    if groups[1]:
        bundles.append(w3lib.write_legacy_bundle(os.path.join(hc, 'buffers0.bundle'), groups[1]))
    w3lib.write_legacy_metadata_store(os.path.join(hc, 'metadata.store'), bundles)
    shutil.copyfile(os.path.join(sc, 'texture.cache'), os.path.join(hc, 'texture.cache'))
    for bn, g in zip(('blob0.bundle', 'buffers0.bundle'), groups):
        bp = os.path.join(hc, bn)
        if g:
            got = {e.name: w3lib.bundle_read(bp, e) for e in w3lib.bundle_entries(bp)}
            assert got == dict(g), 'scalp merge: %s differs after rewrite' % bn
    # the scalp's entry must be byte-identical to its source
    got = {e.name: w3lib.bundle_read(os.path.join(hc, 'blob0.bundle'), e) for e in w3lib.bundle_entries(os.path.join(hc, 'blob0.bundle'))}
    for n, d in extra:
        assert got[n] == d
    return [n for n, _ in extra]


def check_mod_dir(md):
    sys.path.insert(0, os.path.join(ROOT, 'tools'))
    import w3lib
    name = os.path.basename(md)
    c = os.path.join(md, 'content')
    errs = []
    for need in ('blob0.bundle', 'metadata.store'):
        if not os.path.isfile(os.path.join(c, need)):
            errs.append('%s lacks content/%s' % (name, need))
    if errs:
        return errs
    sd = os.path.join(c, 'scripts', 'local')
    if not (os.path.isdir(sd) and any(f.startswith('hwfa_req_') for f in os.listdir(sd))):
        errs.append('%s: Core requirement script missing' % name)
    for f in os.listdir(c):
        if f.endswith('.bundle'):
            b = os.path.join(c, f)
            for e in w3lib.bundle_entries(b):
                if e.name.endswith('.redfur'):
                    data = w3lib.bundle_read(b, e)
                    if b'importFile' in data or b'px.apx' in data:
                        errs.append('%s: %s carries importFile / third-party skeleton metadata' % (name, e.name))
    return errs


def build_allinone(out, aiover, core_version, jobs, det_zip, scan):
    py = sys.executable
    cfgs = configs()
    stage = os.path.join(ROOT, 'build', 'aio', aiover)
    pkg = os.path.join(stage, 'pkg')
    if os.path.isdir(stage):
        shutil.rmtree(stage)
    os.makedirs(pkg)
    env = dict(os.environ, HWFA_MOD_OUT=pkg, HWFA_SKELETON='game')

    def one(t):
        ch, cfg, _mod = t
        log = os.path.join(stage, cfg + '.log')
        with open(log, 'w', encoding='utf-8') as fh:
            r = subprocess.run([py, os.path.join(ROOT, 'tools', 'hairworks', 'hairpipe.py'), cfg, 'build', '--release'] + (['--lore-guard'] if ch == 'triss' else []) + (['--mod', _mod] if ch in AIO_MOD_NAMES else []),
                               cwd=ROOT, env=env, stdout=fh, stderr=subprocess.STDOUT)
        print('  %-10s %-40s %s' % (ch, cfg, 'ok' if r.returncode == 0 else 'FAILED (see %s)' % log), flush=True)
        return r.returncode

    def opt(t):
        key, (cfg, mod) = t
        log = os.path.join(stage, cfg + '.log')
        with open(log, 'w', encoding='utf-8') as fh:
            r = subprocess.run([py, os.path.join(ROOT, 'tools', 'hairworks', 'hairpipe.py'), cfg, 'build', '--release', '--mod-guard', '--mod', mod],
                               cwd=ROOT, env=dict(env, HWFA_MOD_OUT=os.path.join(pkg, OPTIONAL_DIR)), stdout=fh, stderr=subprocess.STDOUT)
        print('  %-10s %-40s %s' % (key, cfg, 'ok (optional)' if r.returncode == 0 else 'FAILED (see %s)' % log), flush=True)
        return r.returncode
    print('All In One: %d release builds (skeleton = game, %d in parallel)' % (len(cfgs), jobs), flush=True)
    with ThreadPoolExecutor(jobs) as ex:
        rcs = list(ex.map(one, cfgs)) + list(ex.map(opt, AIO_OPTIONAL.items()))
    if any(rcs):
        sys.exit('All In One: a build failed')
    game_mods = os.path.join(os.path.dirname(ROOT), 'mods')
    mods = {ch: mod for ch, _c, mod in cfgs}
    for ch, extra in AIO_EXTRA_MODS.items():
        src = os.path.join(game_mods, extra)
        if not os.path.isdir(src):      # not installed in the game: use the scalp_tex.py build folder of the repo
            alt = os.path.join(ROOT, 'build', 'scalp' if ch == 'triss' else 'scalp_' + ch, extra)
            if os.path.isdir(os.path.join(alt, 'content')):
                src = alt
        if not os.path.isdir(src):
            sys.exit('missing %s (build + install it with scalp_tex.py first)' % src)
        print('  scalp %s merged into %s: %s' % (extra, mods[ch], merge_scalp(os.path.join(pkg, mods[ch]), src)), flush=True)
    errs = []
    for _ch, _cfg, mod in cfgs:
        errs += check_mod_dir(os.path.join(pkg, mod))
    for _key, (_cfg, mod) in AIO_OPTIONAL.items():
        md = os.path.join(pkg, OPTIONAL_DIR, mod)
        errs += check_mod_dir(md)
        sd = os.path.join(md, 'content', 'scripts', 'local')
        if not (os.path.isdir(sd) and any(f.startswith('hwfa_') and f.endswith('_guard.ws') for f in os.listdir(sd))):
            errs.append('%s: mod guard script missing' % mod)
    errs += scan(pkg)
    if errs:
        sys.exit('All In One check failed:\n' + '\n'.join(errs))
    chars = '\n'.join('  - ' + AIO_NAMES[ch] for ch in [c for c, _c, _m in cfgs] + list(AIO_OPTIONAL))
    with open(os.path.join(pkg, 'README.txt'), 'w', encoding='utf-8') as fh:
        fh.write(README % dict(v=aiover, chars=chars, core=core_version))
    with open(os.path.join(pkg, 'REQUIREMENTS.txt'), 'w', encoding='utf-8') as fh:
        fh.write('Requires Hairwork for All - Core v%s or newer (Nexus 13996, file "Hairwork for All - Core") and\n'
                 'Graphics Control (Nexus 13349) with PT hair > Hair root transparency mode = Mode 0.\n'
                 'Triss also needs the lore-friendly Triss mod (Nexus 6457), not included.\n'
                 'The optional headband Ciri (optional\\mod000_CiriWavyHairworks) needs "Ciri - long wavy hair" (Nexus 7063), not included.\n' % core_version)
    r = subprocess.run([py, os.path.join(ROOT, 'tools', 'sdk_guard.py'), '--tree', pkg])
    if r.returncode:
        sys.exit('sdk_guard blocked the All In One package')
    files = []
    for dp, _, fns in os.walk(pkg):
        for fn in fns:
            full = os.path.join(dp, fn)
            files.append((full, os.path.relpath(full, pkg).replace('\\', '/')))
    zp = os.path.join(out, 'HairworkForAll_AllInOne_%s.zip' % aiover)
    det_zip(zp, files)
    with zipfile.ZipFile(zp) as zf:
        if zf.testzip():
            sys.exit('All In One zip integrity failed')
        names = zf.namelist()
    # third-party Yennefer mod (modYenneferFurHairworks / fur02.redfur); our own modYenneferHairworks is fine
    bad = [n for n in names if n.lower().endswith('fur02.redfur') or 'yenneferfur' in n.lower()]
    if bad:
        sys.exit('All In One zip holds third-party names: %s' % bad)
    print('%s: %d files, %.2f MB' % (os.path.basename(zp), len(names), os.path.getsize(zp) / 1e6))
    return zp


if __name__ == '__main__':
    import build_release_hairwork as B
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', required=True)
    ap.add_argument('--aio-version', default='0.1.0')
    ap.add_argument('--jobs', type=int, default=3)
    a = ap.parse_args()
    sys.path.insert(0, os.path.join(ROOT, 'core'))
    import hwcore as K
    os.makedirs(a.out, exist_ok=True)
    build_allinone(a.out, a.aio_version, K.CORE_VERSION, a.jobs, B.det_zip, B.scan)
