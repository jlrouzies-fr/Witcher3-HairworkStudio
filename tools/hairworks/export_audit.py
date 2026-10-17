r"""export_audit - does everything the Groom Studio shows reach the fur the game reads?  (2026-10-15)

  py "<repo>\tools\hairworks\export_audit.py" <cfg> [--project <x.groomproj.json>] [--fur <x.redfur | x_HW.json>]
                                              [--source built|installed] [--mod-dir <mods folder>] [--json] [-v]
  py "<repo>\tools\hairworks\export_audit.py" --aio [--workers 5] [--json]   every pick of tools\build_allinone.py

Nothing is installed or written to <game>\mods, the game is never started.
  EXPECTED  = the Studio project's runtime look. Project = config studio_geo.source / studio_geo_beard.source, or a
              *.groomproj.json named in the config note, or --project. Look = the base build's fur (the project config's
              build; the bytes the Studio pack was made from, desk.json tree only as a fallback) with the project's `look`
              dict on top (src/app.cpp loadProject -> applyFurState: look.over holds the material-preset values, the sliders
              and the Studio caps; src/params.cpp Look::get = over, else base). Hair fur = project top level, beard fur =
              project["beard"] (a beard-only build: the top level is the beard fur). A fur the project has no look for (or no
              project at all, i.e. never in the Studio): the keys the config itself sets (hairpipe.look_params incl.
              look.extra_set, beard.material), nothing else.
  ACTUAL    = the pick built NOW with the current code, in memory (hairpipe.build assemble=True; cached in
              build\export_audit\<cfg>.pkl by an input stamp) = what the release build packs. --source installed reads
              <game>\mods\<mod>\content\blob0.bundle instead; --fur takes one .redfur or a *_HW.json pack (tree values).
              The report says whether the installed redfur equals the built pick.
Every leaf of physicalMaterials / graphicalMaterials / levelOfDetail (+ the materialSets copies, splineMultiplier,
autoHideDistance) is decoded straight from the CR2W properties with its real CR2W type. Floats: tolerance 1e-4.
Row status: OK; DIFF (both present, values differ); MISSING (the Studio has the value, the fur has no such property);
MSDIFF (top level right, the materialSets copy differs: inherited from the vanilla material source, not a Studio value).
A "<-" note says where a lost value stopped: not in the config / config has the Studio value / config has another value
(the project was saved again after the config was made).
Textures: every texture handle (slot, path, where it lives: mod bundle / game texture.cache / game bundle / MISSING) against
the Studio base fur's slot, and the baked maps (stiffness / density / colour atlas xbm) decoded and compared with the
Studio PNGs the config names. "fix (add to look.extra_set ...)" prints typed entries for the MISSING rows.
Exit code 0 always (it is a report).
"""
import argparse
import glob
import json
import math
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
GAME = os.path.dirname(ROOT)
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, 'tools'))
sys.path.insert(0, os.path.join(ROOT, 'desktop_studio', 'tools'))

from cr2w_edit import CR2WDoc, Chunk  # noqa: E402
import redfur as RF  # noqa: E402

ROOTS = ('physicalMaterials', 'graphicalMaterials', 'levelOfDetail')
TOL = 1e-4
PRIMS = ('Float', 'Bool', 'Uint8', 'Int8', 'Uint16', 'Int16', 'Uint32', 'Int32', 'Uint64', 'Int64', 'Double')


# ------------------------------------------------------------------------------------------------ reading a fur
def _decode(doc, typ, val):
    """-> (kind, value): kind 'leaf' (value normalised), 'struct' (value = raw bytes) or 'raw'"""
    n = len(val)
    if typ == 'Float' and n == 4:
        return 'leaf', struct.unpack('<f', val)[0]
    if typ in ('Bool', 'Uint8') and n == 1:
        return 'leaf', val[0]
    if typ == 'Int8' and n == 1:
        return 'leaf', struct.unpack('<b', val)[0]
    if typ == 'Uint32' and n == 4:
        return 'leaf', struct.unpack('<I', val)[0]
    if typ == 'Int32' and n == 4:
        return 'leaf', struct.unpack('<i', val)[0]
    if typ in ('Uint16', 'Int16') and n == 2:
        return 'leaf', struct.unpack('<H' if typ == 'Uint16' else '<h', val)[0]
    if typ.startswith('handle:') or typ.startswith('soft:'):
        i = struct.unpack('<i', val)[0]
        if i < 0 and -i - 1 < len(doc.imports):
            return 'leaf', {'@import': doc.imports[-i - 1][0]}
        return 'leaf', {'@import': None}
    if typ in ('Color', 'Vector'):
        d = {doc.names[a]: v for a, b, v in RF.parse_struct(val)}
        keys = ('Red', 'Green', 'Blue', 'Alpha') if typ == 'Color' else ('X', 'Y', 'Z', 'W')
        out = []
        for k in keys:
            raw = d.get(k)
            if raw is None:
                out.append(255 if k == 'Alpha' else 0.0)
            elif typ == 'Color':
                out.append(raw[0])
            else:
                out.append(struct.unpack('<f', raw)[0])
        return 'leaf', out
    if typ.startswith('E') and n == 2 and typ not in ('Int16', 'Uint16'):
        return 'leaf', doc.names[struct.unpack('<H', val)[0]]
    if typ.startswith('array:') or typ in ('Box', 'Matrix'):
        return 'raw', val
    if n >= 3 and val[0] == 0 and val[-2:] == b'\0\0':
        return 'struct', val
    return 'raw', val


def flatten_struct(doc, v, pre, out):
    try:
        items = RF.parse_struct(v)
    except AssertionError:
        out[pre] = ('raw', 'unparsed struct')
        return
    for ni, ti, val in items:
        name, typ = doc.names[ni], doc.names[ti]
        path = pre + '.' + name if pre else name
        kind, d = _decode(doc, typ, val)
        if kind == 'struct':
            flatten_struct(doc, d, path, out)
        elif kind == 'raw':
            out[path] = (typ, 'raw:%d bytes' % len(val))
        else:
            out[path] = (typ, d)


class Fur:
    """values {path: (cr2w type, value)} (roots + materialSets.<root>... + top-level scalars), bytes, source label"""

    def __init__(self, label):
        self.label = label
        self.values = {}
        self.data = None
        self.exact_types = True

    @classmethod
    def from_bytes(cls, data, label):
        f = cls(label)
        f.data = data
        doc = CR2WDoc.parse(data)
        ch = Chunk(doc, 0)
        for r in ROOTS + ('materialSets',):
            p = ch.get(r)
            if p is not None:
                flatten_struct(doc, p.value, r, f.values)
        for k in ('splineMultiplier', 'autoHideDistance', 'boneCount'):
            p = ch.get(k)
            if p is not None:
                kind, d = _decode(doc, doc_type(doc, p), p.value)
                if kind == 'leaf':
                    f.values[k] = (doc_type(doc, p), d)
        return f

    @classmethod
    def from_pack(cls, pack, label):
        """a *_HW.json / desk.json data pack: the cr2w_reader tree (types are not known: '?')"""
        f = cls(label)
        f.exact_types = False
        for r, k in (('physicalMaterials', 'physical'), ('graphicalMaterials', 'graphical'), ('levelOfDetail', 'lod')):
            _flat_tree(pack.get(k) or {}, r, f.values)
        _flat_tree({'physicalMaterials': (pack.get('materialSets') or {}).get('physicalMaterials') or {},
                    'graphicalMaterials': (pack.get('materialSets') or {}).get('graphicalMaterials') or {}}, 'materialSets', f.values)
        for k in ('splineMultiplier', 'autoHideDistance'):
            if k in pack:
                f.values[k] = ('?', pack[k])
        return f

    def imports(self):
        return {p: v['@import'] for p, (t, v) in self.values.items() if isinstance(v, dict) and '@import' in v}


def doc_type(doc, prop):
    t = prop.type
    return doc.names[t] if isinstance(t, int) else t


def _flat_tree(d, pre, out):
    for k, v in d.items():
        p = pre + '.' + k
        if isinstance(v, dict) and 'Red' in v:
            out[p] = ('?', [v['Red'], v['Green'], v['Blue'], v.get('Alpha', 255)])
        elif isinstance(v, dict) and 'X' in v:
            out[p] = ('?', [v['X'], v['Y'], v['Z'], v.get('W', 0.0)])
        elif isinstance(v, dict) and '@import' in v:
            out[p] = ('?', v)
        elif isinstance(v, dict):
            _flat_tree(v, p, out)
        elif isinstance(v, bool):
            out[p] = ('?', int(v))
        else:
            out[p] = ('?', v)


# ------------------------------------------------------------------------------------------------ comparing values
def norm(v):
    """Studio / JSON value -> comparable (lists, numbers, strings)"""
    if isinstance(v, dict):
        if 'Red' in v:
            return [v['Red'], v['Green'], v['Blue'], v.get('Alpha', 255)]
        if 'X' in v:
            return [v['X'], v['Y'], v['Z'], v.get('W', 0.0)]
        if '@import' in v:
            return v
    if isinstance(v, bool):
        return int(v)
    if isinstance(v, (list, tuple)):
        return [norm(x) for x in v]
    return v


def same(a, b, tol=TOL, color3=False):
    if isinstance(a, dict) or isinstance(b, dict):
        return (a or {}).get('@import') == (b or {}).get('@import') if isinstance(a, dict) and isinstance(b, dict) else False
    if isinstance(a, list) and isinstance(b, list):
        if color3:
            a, b = a[:3], b[:3]
        return len(a) == len(b) and all(same(x, y, tol) for x, y in zip(a, b))
    if isinstance(a, str) or isinstance(b, str):
        return str(a) == str(b)
    try:
        return abs(float(a) - float(b)) <= max(tol, 1e-6 * max(abs(float(a)), abs(float(b))))
    except (TypeError, ValueError):
        return a == b


def fmt(v):
    if isinstance(v, list):
        return '[' + ', '.join(fmt(x) for x in v) + ']'
    if isinstance(v, float):
        return '%.6g' % v
    if isinstance(v, dict):
        return '@' + str(v.get('@import'))
    return str(v)


def compare(expected, actual, source, tol=TOL):
    """expected {path: (value, where, typehint)}, actual Fur -> rows
    row = dict(path, status OK|DIFF|MISSING, expected, actual, ms, where, type)"""
    rows = []
    for path, (ev, where, hint) in sorted(expected.items()):
        ev = norm(ev)
        if isinstance(ev, dict) and '@import' in ev:
            continue                                # texture handles: checked in the texture section
        ac = actual.values.get(path)
        ms = actual.values.get('materialSets.' + path) if path.split('.')[0] in ROOTS[:2] else None
        row = dict(path=path, expected=ev, where=where, type=hint, actual=None, ms=None)
        c3 = isinstance(ev, list) and len(ev) == 4 and 'olor' in path
        if ac is None:
            row['status'] = 'MISSING'
        else:
            row['actual'] = ac[1]
            row['actual_type'] = ac[0]
            ok = same(ev, ac[1], tol, c3)
            row['status'] = 'OK' if ok else 'DIFF'
            if ms is not None:
                row['ms'] = ms[1]
                if not same(ev, ms[1], tol, c3):
                    # the top-level value is right but the materialSets copy is not (or both differ): the pipeline writes both
                    # places, so a copy that differs while the top level is right comes from the source fur (vanilla)
                    row['status'] = 'DIFF' if not ok else 'MSDIFF'
                    row['note'] = 'materialSets copy differs'
        rows.append(row)
    return rows


# ------------------------------------------------------------------------------------------------ project / build lookup
def rel(p):
    try:
        return os.path.relpath(p, ROOT).replace('\\', '/')
    except ValueError:          # another drive (scratch cache dir)
        return p


def load_cfg(cfg):
    p = os.path.join(HERE, 'chars', cfg + '.json')
    return json.load(open(p, encoding='utf-8-sig'))


def find_project(C, explicit=None):
    """-> (path, how) of the Groom Studio project the config came from, else (None, why)"""
    if explicit:
        return os.path.abspath(explicit), '--project'
    cands = []
    for k in ('studio_geo', 'studio_geo_beard'):
        s = (C.get(k) or {}).get('source')
        if s:
            cands.append((s, 'config ' + k + '.source'))
    for k in ('note', 'why'):
        for m in re.findall(r'[\w./\\-]+\.groomproj\.json', str(C.get(k) or '')):
            cands.append((m, 'config ' + k))
    for s, how in cands:
        p = s if os.path.isabs(s) else os.path.join(ROOT, s)
        if os.path.exists(p):
            return p, how
        hits = glob.glob(os.path.join(ROOT, 'build', 'desktop_studio', '**', os.path.basename(s)), recursive=True)
        if hits:
            return max(hits, key=os.path.getmtime), how + ' (found by name)'
    if cands:
        return None, 'project file named by the config not found: ' + cands[0][0]
    return None, 'no Studio project (never in the Studio)'


def desk_json(cfg):
    p = os.path.join(ROOT, 'build', 'desktop_studio', cfg, 'desk.json')
    if os.path.exists(p):
        try:
            return json.load(open(p))
        except Exception:
            return None
    return None


def base_fur(cfg, which='hair'):
    """the Studio's base: the project config's build fur (pack's fur_path), fallback desk.json tree. -> (Fur, label, desk)"""
    D = desk_json(cfg)
    fp = None
    if D:
        fp = ((D.get('beard_fur') or {}).get('fur_path') if which == 'beard' else D.get('fur_path'))
    if fp:
        p = os.path.join(ROOT, fp)
        if os.path.exists(p):
            return Fur.from_bytes(open(p, 'rb').read(), rel(p)), rel(p), D
    if D and which == 'hair':
        return Fur.from_pack(D, 'desk.json of ' + cfg), 'desk.json of ' + cfg + ' (tree: stiffnessBone* / types not visible)', D
    if D and which == 'beard' and D.get('beard_fur'):
        return Fur.from_pack(D['beard_fur'], 'desk.json beard of ' + cfg), 'desk.json beard of ' + cfg, D
    return None, 'no base fur for ' + cfg, D


class Pack:
    """the files of a mod: {lower in-game path: (path, bytes)}"""

    def __init__(self, label, files, mtime=None):
        self.label = label
        self.files = {n.lower(): (n, d) for n, d in files}
        self.mtime = mtime

    def redfurs(self):
        return {n: d for n, d in self.files.values() if n.endswith('.redfur')}

    def has(self, name):
        return name.lower() in self.files

    def get(self, name):
        return self.files[name.lower()][1]


def load_installed(mod, mod_dir=None):
    """the mod folder blob0.bundle (default <game>/mods) -> Pack or None"""
    from w3lib import bundle_entries, bundle_read
    b = os.path.join(mod_dir or os.path.join(GAME, 'mods'), mod, 'content', 'blob0.bundle')
    if not os.path.exists(b):
        return None
    return Pack(b, [(e.name, bundle_read(b, e)) for e in bundle_entries(b)], os.path.getmtime(b))


CACHE_DIR = os.path.join(ROOT, 'build', 'export_audit')


def _code_stamp(cfg):
    """key of a build's inputs: the config, its edits, the studio_geo files it names and the build code"""
    import hashlib
    h = hashlib.sha1()
    C = load_cfg(cfg)
    files = [os.path.join(HERE, 'chars', cfg + '.json')]
    for k in ('studio_geo', 'studio_geo_beard'):
        for kk in ('geo', 'look', 'stiffness', 'density_png', 'colour_png'):
            v = (C.get(k) or {}).get(kk)
            if v:
                files.append(v if os.path.isabs(v) else os.path.join(ROOT, v))
    if C.get('edits'):
        files += glob.glob(os.path.join(HERE, 'edits', str(C['edits'])))
    for m in ('hairpipe.py', 'redfur.py', 'studio_geo.py', 'beard.py', 'growth.py', 'cr2w_edit.py'):
        files.append(os.path.join(HERE, m))
    files.append(os.path.join(ROOT, 'desktop_studio', 'tools', 'desk_export.py'))
    for f in files:
        if os.path.exists(f):
            h.update(f.encode() + b'%d' % int(os.path.getmtime(f)) + b'%d' % os.path.getsize(f))
    return h.hexdigest()


def build_pack(cfg, cache_dir=None, log=print):
    """the config built with the CURRENT code, in memory (hairpipe.build assemble=True: nothing installed, mods untouched).
    Cached in build/export_audit/<cfg>.pkl by input stamp. -> Pack"""
    import pickle
    cache_dir = cache_dir or CACHE_DIR
    os.makedirs(cache_dir, exist_ok=True)
    cp = os.path.join(cache_dir, cfg + '.pkl')
    stamp = _code_stamp(cfg)
    if os.path.exists(cp):
        try:
            st, files, mt = pickle.load(open(cp, 'rb'))
            if st == stamp:
                return Pack('built (cache %s)' % rel(cp), files, mt)
        except Exception:
            pass
    import hairpipe as H
    C = H.load_config(cfg)
    files, _bufs, _tpls = H.build(C, log=lambda *a: None, assemble=True)
    files = [(r, d) for r, d in files]
    import time
    pickle.dump((stamp, files, time.time()), open(cp, 'wb'))
    return Pack('built now with the current code', files, time.time())


def pick_fur(furs, comp):
    if comp:
        for n, d in furs.items():
            if os.path.basename(n.replace(chr(92), '/')).lower() == (comp + '.redfur').lower():
                return n, d
    return None, None


# ------------------------------------------------------------------------------------------------ the Studio's effective look
def studio_caps(D, eff):
    """Look::enforceCaps (src/params.cpp): width <= ref width x 1.5, detailLODWidth <= max(ref width, .5) x 1.5, distanceLODEnd
    <= max(base, 100). Applied when a project loads, so the exported value is the capped one."""
    R = (D or {}).get('budget_ref') or {}
    w = R.get('width')
    notes = {}
    caps = {}
    if w is not None:
        caps['physicalMaterials.strandWidth.width'] = float(w) * 1.5
        caps['levelOfDetail.detailLOD.detailLODWidth'] = max(float(w), 0.5) * 1.5
    for p, c in caps.items():
        v = eff.get(p)
        if v is not None and isinstance(v[0], (int, float)) and v[0] > c + 1e-6:
            notes[p] = v[0]
            eff[p] = (c, 'Studio cap (was %s)' % fmt(v[0]), v[2])
    return notes


def effective(base, over, D):
    """base Fur + project look overrides -> {path: (value, where, type)}"""
    eff = {}
    if base is not None:
        for p, (t, v) in base.values.items():
            if p.split('.')[0] in ROOTS or p in ('splineMultiplier', 'autoHideDistance'):
                eff[p] = (v, 'base build', t)
    for p, v in (over or {}).items():
        t = '?'
        if isinstance(v, dict) and 'Red' in v:
            t = 'Color'
        elif isinstance(v, dict) and 'X' in v:
            t = 'Vector'
        elif isinstance(v, float):
            t = 'Float'
        eff[p] = (v, 'Studio', eff.get(p, (None, None, t))[2] if p in eff else t)
    studio_caps(D, eff)
    return eff


# ------------------------------------------------------------------------------------------------ the config's own look (no project)
def cfg_expected(cfg, C, which):
    """the keys the config sets on the fur (hairpipe.look_params incl. look.extra_set / preset rule; beard.material) ->
    {path: (value, where, type)}"""
    out = {}
    notes = []
    if which in ('beard', 'scalp'):
        import beard as B
        o = B.cfg(C)
        if not o:
            return out, ['no beard in the config']
        if which == 'scalp':
            sc = o.get('scalp') or {}
            o = dict(o)
            o.update({k: v for k, v in sc.items() if k not in ('z', 'look')})
            lk = dict(o['look'])
            lk.update(sc.get('look') or {})
            o['look'] = lk
        s = B.material(o, 1.0, log=lambda *a: None, tex=None)['set']
        skip = {('physicalMaterials', 'simulation', 'simulate'), ('physicalMaterials', 'volume', 'usePixelDensity'),
                ('physicalMaterials', 'volume', 'densityTexChannel')}
        if (o['look'] or {}).get('density') is None:
            skip.add(('physicalMaterials', 'volume', 'density'))
            notes.append('beard density is computed from the card area (not checked)')
        for k, (typ, v) in s.items():
            if tuple(k) in skip:
                continue
            out['.'.join(k)] = (v, 'config beard.look', typ)
        return out, notes
    import hairpipe as H
    L = dict(H.DEFAULT_LOOK)
    L.update(C.get('look') or {})
    expl = set((C.get('look') or {}).keys())
    s, _tx = H.look_params(L, 'TEX')
    if L.get('preset'):
        s2 = {}
        for k, v in s.items():
            s2[k] = v
        # preset: only the keys the config sets explicitly (hairpipe.build: sentinel re-run); here: the extra_set + explicit ones
        keep = {}
        Ls = dict(L)
        sent = {}
        for i, (k, v) in enumerate(L.items()):
            if k in expl or k in ('mat_src', 'use_card_texture', 'card_density_only', 'preset', 'extra_set'):
                continue
            if isinstance(v, (int, float)) and not isinstance(v, bool):
                Ls[k] = 98765.0 + i
                sent[98765.0 + i] = k
            elif isinstance(v, (list, tuple)) and v and all(isinstance(x, (int, float)) for x in v):
                Ls[k] = [97000 + i] * len(v)
                sent[float(97000 + i)] = k
        s_sent, _ = H.look_params(Ls, 'TEX')

        def flat(v):
            o = []
            for x in (v if isinstance(v, (list, tuple)) else [v]):
                o += flat(x) if isinstance(x, (list, tuple)) else [x]
            return o
        s = {k: v for k, v in s.items()
             if not any(isinstance(x, (int, float)) and not isinstance(x, bool) and float(x) in sent for x in flat(s_sent.get(k, ())))}
        notes.append('look.preset: only explicit keys checked')
    for k, (typ, v) in s.items():
        if typ.startswith('handle'):
            continue
        out['.'.join(k)] = (v, 'config look', typ)
    if C.get('look', {}).get('hair_rgb'):
        notes.append('look.hair_rgb rewrites textureBrightness in the build')
        out.pop('graphicalMaterials.color.textureBrightness', None)
    if (C.get('split_cards') or {}).get('tint'):
        notes.append('split_cards.tint rewrites textureBrightness in the build')
        out.pop('graphicalMaterials.color.textureBrightness', None)
    if C.get('ref_fur'):
        notes.append('ref_fur: the material comes from the reference fur (look.extra_set is dropped unless studio_geo.look)')
    sl = (C.get('studio_geo') or {}).get('look')
    if sl:
        try:
            lk = json.load(open(os.path.join(ROOT, sl) if not os.path.isabs(sl) else sl))
            for p, (hint, val) in lk.get('set', {}).items():
                out[p] = (val, 'studio_geo.look', hint)
        except Exception as e:
            notes.append('studio_geo.look unreadable: %r' % e)
    if (C.get('growth') or {}).get('head_tex'):
        notes.append('growth.head_tex rewrites density / colour textures')
    return out, notes


# ------------------------------------------------------------------------------------------------ textures
def tex_report(fur, base, pack, C, which, D, project):
    """-> (rows, problems)"""
    rows, problems = [], []
    names = pack.files
    try:
        import texread
        tc = texread.cache().by_name
    except Exception:
        tc = {}
    try:
        import gamefiles as G
    except Exception:
        G = None
    imps = fur.imports()
    bimps = base.imports() if base is not None else {}
    for slot in sorted(set(imps) | set(bimps)):
        if slot.startswith('materialSets.'):
            continue
        p, bp = imps.get(slot), bimps.get(slot)
        ms = imps.get('materialSets.' + slot)
        where = None
        if p:
            lp = p.lower()
            where = 'mod bundle' if lp in names else 'game texture.cache' if lp in tc else (
                'game bundle' if (G is not None and G.exists(p)) else 'MISSING')
        r = dict(slot=slot, path=p, where=where, studio_base=bp, materialSets=ms)
        if p and where == 'MISSING':
            problems.append('%s -> %s not found in the mod bundle, texture.cache or game bundles' % (slot, p))
        if p is None and bp:
            r['status'] = 'Studio base has it, fur has none'
            problems.append('%s: Studio base has %s, fur has no texture' % (slot, bp))
        elif p and bp and p.lower() != bp.lower() and where == 'mod bundle':
            r['status'] = 're-baked map in the mod bundle (Studio base: %s)' % bp
        elif p and bp and p.lower() != bp.lower():
            r['status'] = 'differs from the Studio base (%s)' % bp
        elif p and bp is None:
            r['status'] = 'new in the fur' + (' (mod bundle)' if where == 'mod bundle' else '')
        else:
            r['status'] = 'same as the Studio base' if p else '-'
        if p and ms is not None and ms.lower() != p.lower():
            problems.append('%s: materialSets copy %s != %s' % (slot, ms, p))
        rows.append(r)
    # painted maps vs the Studio's PNGs (config studio_geo)
    sg = (C.get('studio_geo') if which == 'hair' else None) or {}
    maps = []
    for key, suffix, size in (('stiffness', '_stiffness.xbm', 512), ('density_png', '_density.xbm', 2048), ('colour_png', '_colour.xbm', 2048)):
        if sg.get(key):
            maps.append((key, suffix, size, sg[key]))
    for key, suffix, size, png in maps:
        pp = png if os.path.isabs(png) else os.path.join(ROOT, png)
        ent = [n for n in names if n.endswith(suffix)]   # lower-case keys
        r = dict(map=key, png=png, png_exists=os.path.exists(pp), xbm=names[ent[0]][0] if ent else None)
        if not ent:
            problems.append('%s: config names %s but the bundle has no *%s' % (key, png, suffix))
        elif not os.path.exists(pp):
            r['compare'] = 'PNG gone (local build\\ file), cannot compare'
        else:
            try:
                import numpy as np
                from PIL import Image
                import headtex
                x = headtex.read_xbm(names[ent[0]][1])
                dec = headtex.decode_dxt1(x['raw'], x['w'], x['h'])
                dec = np.asarray(dec, float)[..., :3]
                im = Image.open(pp).convert('RGB').resize((x['w'], x['h']), Image.LANCZOS)
                ref = np.asarray(im, float)
                d = np.abs(dec - ref)
                r['compare'] = 'xbm %dx%d vs PNG: mean abs diff per channel %s, max %d' % (x['w'], x['h'], ', '.join('%.2f' % d[..., c].mean() for c in range(3)), int(d.max()))
                r['mean_abs'] = float(d.mean())
                if d.mean() > 6.0:
                    problems.append('%s: baked xbm differs from the Studio PNG (mean abs %.2f)' % (key, d.mean()))
            except Exception as e:
                r['compare'] = 'compare failed: %r' % (e,)
        rows.append(r)
    # project paints a stiffness map -> the fur must carry stiffnessTex / rootStiffnessTex
    if project is not None:
        st = project.get('stiffness') or {}
        painted = any(abs(x - 1.0) > 1e-3 for x in (st.get('s') or []) + (st.get('r') or []))
        has = [s for s in imps if s.endswith('stiffnessTex') or s.endswith('rootStiffnessTex')]
        if painted and not has:
            problems.append('the project paints a stiffness map but the fur has no stiffnessTex')
        if not painted and has and not any((fur_imp or '').lower() in names for fur_imp in [imps.get(h) for h in has]):
            problems.append('the fur has a stiffness texture the project did not paint: ' + ', '.join(has))
        rows.append(dict(map='project stiffness painted', painted=painted, fur_has_stiffnessTex=bool(has)))
    return rows, problems


# ------------------------------------------------------------------------------------------------ one pick
# CR2W types of the fur properties that are not Float / Color / Vector, read from the 117 vanilla furs of the game
# (every one of them has the same type in every fur that carries it). Same table as desktop_studio/tools/desk_export.py.
W3_KEY_TYPES = {'stiffnessBoneEnable': 'Bool', 'stiffnessBoneIndex': 'Uint32', 'stiffnessBoneAxis': 'Uint32',
                'useCollision': 'Bool', 'simulate': 'Bool', 'usePixelDensity': 'Bool', 'castShadows': 'Bool',
                'useBackfaceCulling': 'Bool', 'useViewfrustrumCulling': 'Bool', 'enableDetailLOD': 'Bool',
                'enableDistanceLOD': 'Bool', 'enableLOD': 'Bool', 'priority': 'Uint32', 'clumpNumSubclumps': 'Uint32',
                'diffuseBoneIndex': 'Uint32', 'strandBlendMode': 'EHairStrandBlendModeType'}


def suggest_type(row):
    """CR2W type to write a MISSING value with (real types of the W3-only keys read from vanilla furs; else from the value)"""
    name = row['path'].split('.')[-1]
    if name in W3_KEY_TYPES:
        return W3_KEY_TYPES[name]
    v = row['expected']
    if isinstance(v, list):
        return 'Color' if 'olor' in name else 'Vector'
    if isinstance(v, float):
        return 'Float'
    return None


def config_look(C, kind):
    """{path: value} the pipeline config writes on this fur: look.extra_set (+ studio_geo.look) for the hair fur,
    beard.look.extra_set (+ the studio_geo_beard look file) for the beard fur"""
    out = {}

    def fromfile(rel_):
        try:
            lk = json.load(open(rel_ if os.path.isabs(rel_) else os.path.join(ROOT, rel_)))
            for pth, hv in lk.get('set', {}).items():
                out[pth] = hv[1]
        except Exception:
            pass
    if kind == 'beard':
        for pth, tv in (((C.get('beard') or {}).get('look') or {}).get('extra_set') or {}).items():
            out[pth] = tv[1] if isinstance(tv, (list, tuple)) and len(tv) == 2 and isinstance(tv[0], str) else tv
        if (C.get('studio_geo_beard') or {}).get('look'):
            fromfile(C['studio_geo_beard']['look'])
    elif kind == 'hair':
        for pth, tv in ((C.get('look') or {}).get('extra_set') or {}).items():
            out[pth] = tv[1]
        if (C.get('studio_geo') or {}).get('look'):
            fromfile(C['studio_geo']['look'])
    return out


def attribute(rows, cfgvals, tol=TOL):
    """for every row that is not OK: where did the value get lost? cfg = absent / equal to the Studio value / other value"""
    for r in rows:
        if r['status'] in ('OK', 'MSDIFF') or r['where'] != 'Studio':
            continue
        if r['path'] not in cfgvals:
            r['cfg'] = 'not in the config'
        else:
            cv = norm(cfgvals[r['path']])
            r['cfg'] = 'config has the Studio value' if same(r['expected'], cv, tol, False) else 'config has %s (project changed since?)' % fmt(cv)


def classify(name):
    b = os.path.basename(name.replace(chr(92), '/'))[:-len('.redfur')].lower()
    return 'beard' if b.startswith('hb_') else 'scalp' if b.startswith('scalp') else 'hair'


def audit(cfg, project_path=None, fur_arg=None, mod_dir=None, mod=None, tol=TOL, source='built', cache_dir=None, pack=None):
    C = load_cfg(cfg)
    mod = mod or C.get('mod') or 'mod%sHairworks' % cfg.split('_')[0].capitalize()
    res = dict(cfg=cfg, mod=mod, project=None, project_how=None, furs=[], notes=[], source=source)
    pj, how = find_project(C, project_path)
    res['project_how'] = how
    P = None
    if pj:
        P = json.load(open(pj, encoding='utf-8-sig'))
        res['project'] = rel(pj) if pj.startswith(ROOT) else pj
        res['project_mtime'] = int(os.path.getmtime(pj))
        res['project_config'] = P.get('config')
        res['project_material'] = P.get('material')
        res['project_keep_colours'] = P.get('keep_colours')
    res['config_mtime'] = int(os.path.getmtime(os.path.join(HERE, 'chars', cfg + '.json')))
    pack_arg = None
    inst = load_installed(mod, mod_dir)
    if fur_arg:
        if fur_arg.lower().endswith('.redfur'):
            pack = Pack(fur_arg, [(os.path.basename(fur_arg), open(fur_arg, 'rb').read())], os.path.getmtime(fur_arg))
        else:
            pack_arg = json.load(open(fur_arg))
            pack = Pack(fur_arg, [])
    elif source == 'installed':
        pack = inst
        if pack is None:
            res['notes'].append('not installed: mods/%s' % mod)
            return res
    elif pack is None:
        pack = build_pack(cfg, cache_dir)
    res['fur_source'] = pack.label
    res['source_mtime'] = int(pack.mtime) if pack.mtime else None
    furs = pack.redfurs()
    if inst is not None and source != 'installed':
        same_ = {n: (inst.get(n) == d) for n, d in furs.items() if inst.has(n)}
        res['installed'] = ('installed mods/%s (%s): ' % (mod, __import__('time').strftime('%m-%d %H:%M', __import__('time').localtime(inst.mtime))) +
                            ('redfur identical to the pick' if same_ and all(same_.values()) else
                             'redfur DIFFERS from the pick (stale install)' if same_ else 'no matching redfur (other pick installed)'))
    elif inst is None and source != 'installed':
        res['installed'] = 'not installed in <game>/mods/%s' % mod
    if not furs and pack_arg is None:
        res['notes'].append('no redfur in the fur source')
        return res
    D0 = desk_json(P['config']) if P else None
    beard_only = bool(D0) and (str(D0.get('comp') or '').lower().startswith('hb_') or classify_fp(D0.get('fur_path')) == 'beard')
    items = []                                          # (redfur name, bytes|None)
    if pack_arg is not None:
        items.append((fur_arg, None))
    else:
        items = sorted(furs.items(), key=lambda x: ({'hair': 0, 'beard': 1, 'scalp': 2}[classify(x[0])], x[0]))
    for name, data in items:
        kind = 'hair' if pack_arg is not None else classify(name)
        entry = dict(component=kind, redfur=name, rows=[], textures=[], problems=[], notes=[])
        res['furs'].append(entry)
        fur = Fur.from_pack(pack_arg, fur_arg) if data is None else Fur.from_bytes(data, name)
        entry['redfur_bytes'] = len(data) if data else None
        part = bcfg = None
        if P is not None:
            if kind == 'hair' and not beard_only:
                part, bcfg, which = P, P['config'], 'hair'
            elif kind == 'beard' and beard_only:
                part, bcfg, which = P, P['config'], 'hair'          # the pack's main fur IS the beard
            elif kind == 'beard' and isinstance(P.get('beard'), dict) and (D0 or {}).get('beard_fur'):
                part, bcfg, which = P['beard'], P['config'], 'beard'
        if part is not None:
            base, blabel, D = base_fur(bcfg, which)
            entry['base'] = blabel
            if base is None:
                entry['problems'].append(blabel)
                continue
            over = part.get('look') or {}
            eff = effective(base, over, D)
            if which == 'hair' and (C.get('studio_geo') or {}).get('density_png') and 'physicalMaterials.volume.densityTexChannel' in eff:
                eff['physicalMaterials.volume.densityTexChannel'] = ('RED', 'by design: re-baked density atlas (studio_geo.density_png)', 'EHairTextureChannel')
            entry['expected_from'] = 'Studio project look (%d overrides) on %s' % (len(over), blabel)
            entry['n_overrides'] = len(over)
            if not base.exact_types:
                entry['notes'].append('base read from the desk.json tree: stiffnessBone* / types not visible')
        else:
            exp, notes = cfg_expected(cfg, C, kind)
            eff, base, D = exp, None, None
            entry['expected_from'] = 'config look of %s (%d keys it sets)%s' % (
                cfg, len(exp), '' if P is None else '; this fur has no Studio look in the project')
            entry['notes'] += notes
        rows = compare(eff, fur, 'studio' if part is not None else 'config', tol)
        entry['rows'] = rows
        attribute(rows, config_look(C, kind if kind != 'scalp' else 'x'), tol)
        entry['fix_extra_set'] = {r['path']: [suggest_type(r), r['expected']] for r in rows if r['status'] == 'MISSING' and suggest_type(r)}
        entry['unfixable'] = [r['path'] for r in rows if r['status'] == 'MISSING' and not suggest_type(r)]
        entry['n_checked'] = len(rows)
        for k, st in (('n_ok', 'OK'), ('n_diff', 'DIFF'), ('n_msdiff', 'MSDIFF'), ('n_missing', 'MISSING')):
            entry[k] = sum(r['status'] == st for r in rows)
        if part is not None:
            extra = [p for p, (t, v) in fur.values.items() if p.split('.')[0] in ROOTS and p not in eff]
            entry['n_fur_only'] = len(extra)
            entry['fur_only'] = extra
        ms_diff = [p for p, (t, v) in fur.values.items()
                   if p.startswith('materialSets.') and p[len('materialSets.'):] in fur.values and
                   not same(norm(v), norm(fur.values[p[len('materialSets.'):]][1]), tol)]
        entry['materialSets_vs_top'] = [(p[len('materialSets.'):], fmt(fur.values[p[len('materialSets.'):]][1]), fmt(fur.values[p][1])) for p in ms_diff]
        if data is not None:
            tr, tp = tex_report(fur, base, pack, C, 'hair' if kind != 'beard' else 'beard', D, part)
            entry['textures'] = tr
            entry['problems'] += tp
    return res


def classify_fp(fp):
    return classify(fp) if fp else None


# ------------------------------------------------------------------------------------------------ printing
def print_report(res, verbose=False, out=print):
    out('=' * 100)
    out('%s   mod %s' % (res['cfg'], res['mod']))
    out('  project : %s  [%s]' % (res.get('project') or '-', res['project_how']))
    if res.get('project'):
        import time
        out('            config %s, material %r, keep_colours %s' % (res.get('project_config'), res.get('project_material'), res.get('project_keep_colours')))
        out('            project saved %s, config file %s' % (
            time.strftime('%m-%d %H:%M', time.localtime(res['project_mtime'])), time.strftime('%m-%d %H:%M', time.localtime(res['config_mtime']))))
    out('  fur     : %s' % res.get('fur_source'))
    if res.get('installed'):
        out('  install : %s' % res['installed'])
    for n in res['notes']:
        out('  note: ' + n)
    for f in res['furs']:
        out('  --- %s fur %s' % (f['component'], f.get('redfur')))
        out('      expected from: %s' % f.get('expected_from'))
        for n in f['notes']:
            out('      note: ' + n)
        if 'n_checked' in f:
            out('      checked %d values: %d OK, %d DIFF, %d MISSING, %d materialSets-copy-only%s' % (
                f['n_checked'], f['n_ok'], f['n_diff'], f['n_missing'], f['n_msdiff'],
                ('; %d fur-only (not in the Studio look)' % f['n_fur_only']) if f.get('n_fur_only') else ''))
        bad = [r for r in f['rows'] if r['status'] != 'OK']
        shown = f['rows'] if verbose else bad
        if shown:
            out('      %-8s %-58s %-22s %-22s %s' % ('status', 'path', 'expected', 'in fur', 'expected from'))
            for r in shown:
                ac = 'no such property' if r['status'] == 'MISSING' else fmt(r['actual'])
                if r.get('ms') is not None and r['status'] in ('DIFF', 'MSDIFF') and r.get('note'):
                    ac += ' (materialSets ' + fmt(r['ms']) + ')'
                out('      %-8s %-58s %-22s %-22s %s%s%s' % (r['status'], r['path'], fmt(r['expected'])[:22], ac[:44], r['where'],
                                                           ' [%s]' % r['type'] if r['status'] == 'MISSING' else '',
                                                           ('  <- ' + r['cfg']) if r.get('cfg') else ''))
        if f.get('fur_only'):
            out('      fur-only values (not in the Studio look): ' + ', '.join(x.split('.', 1)[1] for x in f['fur_only'][:8]) +
                (' ...' if len(f['fur_only']) > 8 else ''))
        if f.get('materialSets_vs_top'):
            out('      materialSets copy != top level (as built; the game reads both): ' +
                '; '.join('%s %s | %s' % x for x in f['materialSets_vs_top'][:8]) + (' ...' if len(f['materialSets_vs_top']) > 8 else ''))
        if f['textures']:
            out('      textures:')
            for t in f['textures']:
                if 'slot' in t:
                    out('        %-52s %-12s %s  (%s)' % (t['slot'], t['where'] or '-', t['path'] or '-', t['status']))
                elif 'png' in t:
                    out('        map %-12s xbm %s  png %s  %s' % (t['map'], t['xbm'], t['png'], t.get('compare', '')))
                else:
                    out('        %s: painted=%s, fur has stiffnessTex=%s' % (t['map'], t['painted'], t['fur_has_stiffnessTex']))
        if f.get('fix_extra_set'):
            out('      fix (add to look.extra_set%s of the config): %s' % ('' if f['component'] != 'beard' else ' / beard.look.extra_set', json.dumps(f['fix_extra_set'])))
        if f.get('unfixable'):
            out('      MISSING without a known CR2W type: ' + ', '.join(f['unfixable']))
        for p in f['problems']:
            out('      PROBLEM: ' + p)


def summary_row(res):
    chk = sum(f.get('n_checked', 0) for f in res['furs'])
    ok = sum(f.get('n_ok', 0) for f in res['furs'])
    dif = sum(f.get('n_diff', 0) for f in res['furs'])
    mis = sum(f.get('n_missing', 0) for f in res['furs'])
    prob = sum(len(f['problems']) for f in res['furs'])
    return chk, ok, dif, mis, prob


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('cfg', nargs='*')
    ap.add_argument('--project')
    ap.add_argument('--fur', help='a .redfur, or a *_HW.json data pack (tree values: no exact types)')
    ap.add_argument('--source', choices=('built', 'installed'), default='built',
                    help='built = the config built now with the current code, in memory (default; cached in build/export_audit); '
                         'installed = <game>/mods/<mod>')
    ap.add_argument('--mod-dir', dest='mod_dir', help='folder holding the mod folders (default <game>/mods)')
    ap.add_argument('--mod', help='mod folder name (default: the config mod key)')
    ap.add_argument('--aio', action='store_true', help='every pick of tools/build_allinone.py (AIO_ORDER + optional)')
    ap.add_argument('--workers', type=int, default=1, help='parallel builds for --aio (each build is a separate process)')
    ap.add_argument('--build-only', dest='build_only', action='store_true', help='just build + cache the packs')
    ap.add_argument('--cache-dir', dest='cache_dir')
    ap.add_argument('--json', action='store_true')
    ap.add_argument('-v', '--verbose', action='store_true', help='print OK rows too')
    ap.add_argument('--tol', type=float, default=TOL)
    a = ap.parse_args()
    jobs = []
    if a.aio:
        sys.path.insert(0, os.path.join(ROOT, 'tools'))
        import build_allinone as B
        for ch, cfg, mod in B.configs():
            jobs.append((cfg, mod))
        for k, (cfg, mod) in B.AIO_OPTIONAL.items():
            jobs.append((cfg, mod))
    for c in a.cfg:
        jobs.append((c, a.mod))
    if not jobs:
        ap.error('give a config name or --aio')
    if a.build_only:
        for cfg, _m in jobs:
            p = build_pack(cfg, a.cache_dir)
            print(cfg, p.label, len(p.files), 'files')
        return
    if a.source == 'built' and not a.fur and len(jobs) > 1 and a.workers > 1:
        import subprocess
        from concurrent.futures import ThreadPoolExecutor
        todo = []
        for cfg, _m in jobs:
            cp = os.path.join(a.cache_dir or CACHE_DIR, cfg + '.pkl')
            ok = False
            if os.path.exists(cp):
                import pickle
                try:
                    ok = pickle.load(open(cp, 'rb'))[0] == _code_stamp(cfg)
                except Exception:
                    ok = False
            if not ok:
                todo.append(cfg)

        def run(cfg):
            cmd = [sys.executable, os.path.abspath(__file__), '--build-only', cfg] + (['--cache-dir', a.cache_dir] if a.cache_dir else [])
            r = subprocess.run(cmd, capture_output=True, text=True)
            print('built %s (rc %d)' % (cfg, r.returncode), file=sys.stderr)
            return r.returncode
        with ThreadPoolExecutor(a.workers) as ex:
            list(ex.map(run, todo))
    results = []
    for cfg, mod in jobs:
        try:
            r = audit(cfg, a.project, a.fur, a.mod_dir, mod, a.tol, a.source, a.cache_dir)
        except Exception as e:
            import traceback
            r = dict(cfg=cfg, mod=mod, project=None, project_how='ERROR', furs=[], notes=['audit failed: %r' % (e,), traceback.format_exc()])
        results.append(r)
        if not a.json:
            print_report(r, a.verbose)
    if a.json:
        print(json.dumps(results, indent=1, default=str))
    elif len(results) > 1:
        print('=' * 100)
        print('%-34s %-8s %8s %6s %6s %8s %6s' % ('pick', 'project', 'checked', 'OK', 'DIFF', 'MISSING', 'tex!'))
        for r in results:
            chk, ok, dif, mis, prob = summary_row(r)
            print('%-34s %-8s %8d %6d %6d %8d %6d' % (r['cfg'], 'yes' if r.get('project') else 'no', chk, ok, dif, mis, prob))


if __name__ == '__main__':
    main()
