r"""roster - the full list of named (main / story) characters for the HairWorks project, from the game files.

  py tools\hairworks\roster.py            -> tools\hairworks\roster.json (tracked) + a summary
  py tools\hairworks\roster.py --check    analysis only, prints the per-mesh evidence, writes nothing
  py tools\hairworks\roster.py --geralt   (re)write only the `geralt` entry (hairstyles from the hair items) in place
  py tools\hairworks\roster.py --geralt-baselines   chars\geralt<style>_baseline.json per hairstyle (never overwrites)
  py tools\hairworks\roster.py --beards [a,b]      add / refresh only the `beard` field (type cards|painted|none, mesh, card and
                                                     triangle counts, evidence) in roster.json, in place
  py tools\hairworks\roster.py --hairstyles [a,b]   add / refresh only the `hairstyles` lists (appearances grouped by
                                                     hair component) in roster.json, in place; Geralt's are kept

How the list was built (2026-10-11): every `characters\npc_entities\{main_npc,secondary_npc}\*.w2ent` and
`quests\{main_npcs,secondary_npcs}\*.w2ent` of the base game, Hearts of Stone (dlc\ep1) and Blood and Wine (dlc\bob)
was read (display name from en.w3strings, appearances, meshes); the story characters were picked from that survey
by hand (ROSTER below). Names that do not match the file names: Thaler = talar, Birna = lady_bran, Anna Strenger =
barons_wife_celina (appearance is literally called "tamara"), Whoreson Junior = cyprian_willey, Gaunter O'Dimm =
mirror_man / mr_mirror, Ermion = mousesack, Caranthir = caranthil (appearance "canaris"), Cerys = cerys (appearance
"becca"), Marlene = old_woman_wight (cured = old_woman_wight_fed), Vlodimir = olgierd_brother (appearance "witold").
Added by hand (2026-10-11, user): Johnny the godling (quests\secondary_npcs\godling_johnny.w2ent, appearance godling_boy;
portrait painted from the user's screenshot, make_portraits.py USER_SOURCES: too small for the capture framing).
Removed (2026-10-11, user): Uma, a small monster, not needed; EXCLUDED keeps him out of every regeneration.

Per character the generator records the entity template, the default appearance (clean: not wounded, not disguised,
no helmet when a face-visible one exists) and an automatic hair analysis of that appearance:
  cards        a hair-card mesh (pbr_hair material, not a beard / brow / lash) reaching the top of the head
  scalp_beard  hair only inside the beard mesh (hb_*) which also covers the scalp (dwarves, ...): beards stay
               vanilla, so nothing to convert without splitting that mesh
  solid        a solid (pbr_std) c_* hair mesh, no cards (Roche)
  bald         no hair geometry on the scalp (bald, shaved, or very short hair painted on the head texture)
  headgear     a non-hair mesh above the top of the head (hat, hood, helmet, crown)
`hair_type` is the curated verdict (auto value unless ROSTER overrides it): cards | tied_cards (bun / braid / ponytail:
stays vanilla cards, rules.md) | scalp_beard | solid | bald | helmet (the face-visible look is still shot) |
native_hairworks (no human NPC except Geralt, added 2026-10-11 with one `hairstyles` entry per barber item, see
GERALT_STYLES; monsters / bears; Johnny the godling: a
CFurComponent i_01__godling + its redfur, the pbr_hair card mesh is what shows with HairWorks off).
`status` = existing (one of the 27 HairWorks characters, see characters.md) or not_started.
"""
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

OUT = os.path.join(HERE, 'roster.json')

# key: dict(name, tpl, app, part, sex, [alt], [hair_type], [note]). key = HairLab character name (no '_').
# part: base | hos | baw. Existing project characters keep their HairLab baseline template / appearance.
EXISTING = {
    'anna': ('Anna Henrietta', 'baw', 'f'), 'avallach': ("Avallac'h", 'base', 'm'), 'crach': ('Crach an Craite', 'base', 'm'),
    'dandelion': ('Dandelion', 'base', 'm'), 'dettlaff': ('Dettlaff', 'baw', 'm'), 'emhyr': ('Emhyr var Emreis', 'base', 'm'),
    'eredin': ('Eredin', 'base', 'm'), 'eskel': ('Eskel', 'base', 'm'), 'fringilla': ('Fringilla Vigo', 'base', 'f'),
    'guillaume': ('Guillaume', 'baw', 'm'), 'hjalmar': ('Hjalmar', 'base', 'm'), 'iris': ('Iris von Everec', 'hos', 'f'),
    'keira': ('Keira Metz', 'base', 'f'), 'lambert': ('Lambert', 'base', 'm'), 'margarita': ('Margarita Laux-Antille', 'base', 'f'),
    'olgierd': ('Olgierd von Everec', 'hos', 'm'), 'oriana': ('Orianna', 'baw', 'f'), 'priscilla': ('Priscilla', 'base', 'f'),
    'regis': ('Regis', 'baw', 'm'), 'shani': ('Shani', 'hos', 'f'), 'sheala': ('Sile de Tancarville', 'base', 'f'),
    'tamara': ('Tamara Strenger', 'base', 'f'), 'triss': ('Triss Merigold', 'base', 'f'), 'ves': ('Ves', 'base', 'f'),
    'vesemir': ('Vesemir', 'base', 'm'), 'vivienne': ('Vivienne de Tabris', 'baw', 'f'), 'yennefer': ('Yennefer', 'base', 'f'),
}

E = 'characters\\npc_entities\\'
Q = 'quests\\'
BOB = 'dlc\\bob\\data\\'
EP1 = 'dlc\\ep1\\data\\'
ROSTER = {
    # ---- base game
    'ciri': dict(name='Ciri', tpl=E + 'main_npc\\ciri.w2ent', app='ciri', part='base', sex='f', hair_type='tied_cards',
                 alt=['ciri_winter', '__q505_hooded (c_06, hood)', 'dlc11 ciri_dlc.w2ent / ciri_dlc (alternative look, same c_01 cards)'],
                 note='c_01 tied back (bun / ponytail): stays vanilla cards (rules.md; every strand attempt collapsed)'),
    'zoltan': dict(name='Zoltan Chivay', tpl=E + 'main_npc\\zoltan.w2ent', app='zoltan', part='base', sex='m'),
    'roche': dict(name='Vernon Roche', tpl=E + 'main_npc\\roche.w2ent', app='roche', part='base', sex='m', hair_type='helmet',
                  note='c_01_ma__roche (pbr_std) is his black hood, in the only appearance (L235)'),
    'philippa': dict(name='Philippa Eilhart', tpl=E + 'secondary_npc\\filippa_eilhart.w2ent', app='filippa', part='base',
                     sex='f', hair_type='tied_cards', alt=['__q310_no_eyes (blinded)', 'filippa_cut_hand'],
                     note='tied (characters.md: 35 guides, stays cards)'),
    'radovid': dict(name='Radovid V', tpl=E + 'main_npc\\radovid.w2ent', app='radovid', part='base', sex='m'),
    'dijkstra': dict(name='Sigismund Dijkstra', tpl=E + 'main_npc\\djikstra.w2ent', app='djikstra', part='base', sex='m',
                     alt=['__q302_djikstra_bathhouse']),
    'thaler': dict(name='Thaler', tpl=E + 'main_npc\\talar.w2ent', app='talar', part='base', sex='m'),
    'bedlam': dict(name='Francis Bedlam (King of Beggars)', tpl=E + 'secondary_npc\\francis_bedlam.w2ent', app='francis_bedlam',
                   part='base', sex='m', alt=['__q302_francis_bedlam_bathhouse']),
    'carlo': dict(name='Carlo Varese (Cleaver)', tpl=E + 'secondary_npc\\karl_varese.w2ent', app='karl_varese', part='base',
                  sex='m', alt=['__q302_karl_varese_bathhouse']),
    'junior': dict(name='Whoreson Junior (Cyprian Wiley)', tpl=E + 'secondary_npc\\cyprian_willey.w2ent', app='cyprian_willey',
                   part='base', sex='m', alt=['__q302_beaten', '__q310_poor']),
    'menge': dict(name='Caleb Menge', tpl=E + 'secondary_npc\\menge.w2ent', app='menge', part='base', sex='m'),
    'graden': dict(name='Graden', tpl=Q + 'secondary_npcs\\graden.w2ent', app='__q103_graden', part='base', sex='m',
                   note='the template is a witch-hunter pool (17 appearances); __q103_graden is Graden; hair = generic c_11_ma__hair (elihal haircut)'),
    'dudu': dict(name='Dudu Biberveldt', tpl=E + 'secondary_npc\\dudu.w2ent', app='dudu', part='base', sex='m',
                 alt=['dudu_beaten']),
    'corinne': dict(name='Corinne Tilly', tpl=E + 'secondary_npc\\corine_dreamer.w2ent', app='dreamer', part='base', sex='f'),
    'baron': dict(name='Phillip Strenger (Bloody Baron)', tpl=E + 'main_npc\\baron.w2ent', app='baron', part='base', sex='m',
                  hair_type='helmet', note='hh_01 (pbr_hair) is his leather hood, worn in every appearance (L235)',
                  alt=['baron_armed', 'baron_fistfight (bruised)']),
    'annastrenger': dict(name='Anna Strenger', tpl=Q + 'secondary_npcs\\barons_wife_celina.w2ent', app='tamara', part='base',
                         sex='f', alt=['anna_marked'], note='the clean appearance is named "tamara" in the file'),
    'johnny': dict(name='Johnny (godling)', tpl=Q + 'secondary_npcs\\godling_johnny.w2ent', app='godling_boy', part='base',
                   sex='m', no_portrait=True,
                   note='godling of Crookback Bog (Ladies of the Woods); hair i_01__godling = a CFurComponent (vanilla redfur '
                        'i_01__godling_fur, 445 guides) whose mesh is pbr_hair cards (shown with HairWorks off); no '
                        'capture (too small for the framing): portrait painted from the user\'s screenshot'),
    'letho': dict(name='Letho of Gulet', tpl=E + 'secondary_npc\\letho.w2ent', app='letho', part='base', sex='m'),
    'voorhis': dict(name='Morvran Voorhis', tpl=E + 'secondary_npc\\voorhis.w2ent', app='voorhis', part='base', sex='m'),
    'geels': dict(name="Ge'els", tpl=E + 'main_npc\\geels.w2ent', app='geels__civil', part='base', sex='m'),
    'imlerith': dict(name='Imlerith', tpl=E + 'main_npc\\imlerith.w2ent', app='__q111_no_helmet', part='base', sex='m',
                     hair_type='helmet', alt=['imlerith (default, helmet)', 'imlerith_burned'],
                     note='default appearance wears the helmet; the bare-head look __q111_no_helmet is shot'),
    'caranthir': dict(name='Caranthir', tpl=E + 'main_npc\\caranthil.w2ent', app='canaris', part='base', sex='m',
                      hair_type='helmet',
                      note='one body mesh with the closed skull helmet, no face-visible appearance: no portrait (L235)'),
    'cerys': dict(name='Cerys an Craite', tpl=E + 'main_npc\\cerys.w2ent', app='becca', part='base', sex='f',
                  hair_type='tied_cards', alt=['__q208_crown'], note='braided: stays cards (characters.md, R77 wild mop)'),
    'ermion': dict(name='Ermion (Mousesack)', tpl=E + 'main_npc\\mousesack.w2ent', app='mousesack', part='base', sex='m', hair_type='helmet',
                   note='c_01_ma__mousesack (pbr_std) is his antlered druid hat (L235); the long beard stays vanilla; quest copy quests\\main_npcs\\mousesack.w2ent'),
    'birna': dict(name='Birna Bran', tpl=E + 'secondary_npc\\lady_bran.w2ent', app='lady_bran', part='base', sex='f',
                  hair_type='tied_cards', note='braided updo (L235): stays cards'),
    'svanrige': dict(name='Svanrige', tpl=E + 'secondary_npc\\svanrige.w2ent', app='svanrige', part='base', sex='m',
                     alt=['__q208_crown']),
    'udalryk': dict(name='Udalryk', tpl=E + 'secondary_npc\\uldaryk.w2ent', app='uldaryk', part='base', sex='m',
                    alt=['__q203_eye_patch', '__q203_no_eye', '__q203_band']),
    'donar': dict(name='Donar an Hindar', tpl=E + 'secondary_npc\\donar_an_hindar.w2ent', app='donar_an_hindar', part='base', sex='m'),
    'lugos': dict(name='Madman Lugos', tpl=E + 'secondary_npc\\lugos_the_mad.w2ent', app='lugos', part='base', sex='m',
                  alt=['lugos_fistfight']),
    # ---- Hearts of Stone
    'gaunter': dict(name="Gaunter O'Dimm", tpl=EP1 + 'characters\\npc_entities\\main_npc\\mirror_man.w2ent', app='mirror_man',
                    part='hos', sex='m', alt=['mirror_man_wedding', 'base game characters\\npc_entities\\secondary_npc\\mirror_man.w2ent'],
                    note='quest copies dlc\\ep1\\data\\quests\\main_npcs\\mr_mirror_ep1.w2ent and quests\\secondary_npcs\\mr_mirror.w2ent'),
    'ewald': dict(name='Ewald Borsodi', tpl=EP1 + 'characters\\npc_entities\\main_npc\\ewald_borsody.w2ent', app='ewald_borsody',
                  part='hos', sex='m', alt=['ewald_borsody_hood', 'ewald_borsody_disguised', 'ewald_borsody_mask']),
    'horst': dict(name='Horst Borsodi', tpl=EP1 + 'characters\\npc_entities\\main_npc\\horst_borsody.w2ent', app='horst_borsody',
                  part='hos', sex='m'),
    'vlodimir': dict(name='Vlodimir von Everec', tpl=EP1 + 'quests\\secondary_npcs\\olgierd_brother.w2ent', app='witold_no_ghost',
                     part='hos', sex='m', alt=['witold (ghost: two head meshes)']),
    'eveline': dict(name='Eveline Gallo', tpl=EP1 + 'characters\\npc_entities\\secondary_npc\\crewmember_eveline.w2ent',
                    app='eveline_gallo', part='hos', sex='f', alt=['eveline_gallo_mask'],
                    note='hair = c_18_wa__ves_dark (the dark twin of Ves\'s haircut): a Ves build may carry over'),
    # ---- Blood and Wine
    'syanna': dict(name='Syanna', tpl=BOB + 'characters\\npc_entities\\main_npc\\syanna.w2ent', app='syanna', part='baw', sex='f',
                   hair_type='tied_cards', alt=['syanna_red_hood_on (c_02, hood)', 'syanna_ribbon'],
                   note='tied (characters.md): stays cards'),
    'damien': dict(name='Damien de la Tour', tpl=BOB + 'characters\\npc_entities\\main_npc\\damien_de_la_tour.w2ent', app='olivier',
                   part='baw', sex='m', alt=['scars', 'mourning', 'massacred'], note='the clean appearance is named "olivier" in the file'),
    'palmerin': dict(name='Palmerin de Launfal', tpl=BOB + 'characters\\npc_entities\\secondary_npc\\palmerin.w2ent', app='palmerin',
                     part='baw', sex='m', alt=['palmerin_helmet', 'palmerin_helmet_open']),
    'milton': dict(name='Milton de Peyrac-Peyran', tpl=BOB + 'characters\\npc_entities\\secondary_npc\\milton.w2ent', app='milton',
                   part='baw', sex='m', alt=['milton_helmet', 'milton_helmet_open']),
    'marlene': dict(name='Marlene de Trastamara', tpl=BOB + 'characters\\npc_entities\\secondary_npc\\old_woman_wight.w2ent',
                    app='old_woman_wight_fed', part='baw', sex='f', alt=['old_woman_wight_01 (the wight)'],
                    note='the cured old woman (old_woman_wight_fed); the wight form is a monster'),
}
EXCLUDED = {
    'ciri_young': 'child flashback model (c_05_cwa__ciri_young); not a roster character',
    'uma': 'Uma, the cursed form (a small monster): not needed (user, 2026-10-11)',
}


def _bbox(mesh):
    import gamefiles as G
    try:
        bb = G.tree(mesh)[0]['boundingBox']
        return float(bb['Min']['Z']), float(bb['Max']['Z'])
    except Exception:
        return None


def analyse(tpl, app):
    """-> dict(auto=<type>, evidence=[...], hair_templates=[...], beards=[...])"""
    import discover as D
    import gamefiles as G
    BS = '\\'
    apps = D.appearances(tpl)
    if app not in apps:
        raise SystemExit('%s: appearance %r not in %s' % (tpl, app, sorted(apps)))
    rows, heads = [], []
    for inc in apps[app]:
        for comp, cls, mesh, fm in D.mesh_comps(inc):
            k = D.classify(mesh)
            try:
                mats = sorted({os.path.basename(b.replace(BS, '/')) for b, _ in G.materials(mesh)})
            except Exception:
                mats = []
            z = _bbox(mesh)
            rows.append(dict(tpl=inc, comp=comp, cls=cls, mesh=mesh, kind=k, mats=mats, z=z, fur=fm))
            if k == 'head' and z:
                heads.append(z[1])
    if not heads:                         # h_01__uma: no race tag, not matched by discover.classify
        heads = [r['z'][1] for r in rows if r['z'] and os.path.basename(r['mesh']).startswith('h_')]
    top = max(heads) if heads else None
    ev, hair_tpls, beards, kinds = [], [], [], set()
    hats = []                             # headgear meshes (desk_prep: the Studio's Show headgear view)
    base = lambda x: x.replace(BS, '/').rsplit('/', 1)[-1]
    for r in rows:
        z = r['z']
        if r['cls'] == 'CFurComponent' or r['fur']:
            kinds.add('native_hairworks')
            ev.append('fur %s' % base(r['fur'] or r['mesh']))
        if not z:
            continue
        on_top = top is not None and z[1] > top - 0.05 and z[0] > top - 0.6
        if r['kind'] == 'hair' and on_top:
            kinds.add('cards')
            if r['tpl'] not in hair_tpls:
                hair_tpls.append(r['tpl'])
            ev.append('cards %s (z %.2f..%.2f)' % (base(r['mesh']), z[0], z[1]))
        elif r['kind'] == 'beard':
            beards.append(base(r['mesh']))
            if top is not None and z[1] > top - 0.03:
                kinds.add('scalp_beard')
                ev.append('beard mesh over the scalp %s (z top %.2f, head top %.2f)' % (base(r['mesh']), z[1], top))
        elif r['kind'] in ('other', 'body') and top is not None and z[1] > top + 0.005 and z[0] > top - 0.45:
            b = base(r['mesh'])
            if b.startswith('c_') and not any('pbr_hair' in m for m in r['mats']):
                kinds.add('solid')
                ev.append('solid hair mesh %s %s' % (b, r['mats']))
            else:
                kinds.add('headgear')
                if r['mesh'] not in hats:
                    hats.append(r['mesh'])
                ev.append('above the head %s (z top %.2f, head top %.2f)' % (b, z[1], top))
    if top is None:
        ev.append('no separate head mesh')
    auto = ('native_hairworks' if 'native_hairworks' in kinds else 'cards' if 'cards' in kinds else
            'solid' if 'solid' in kinds else 'scalp_beard' if 'scalp_beard' in kinds else 'bald')
    return dict(auto=auto, headgear='headgear' in kinds, headgear_meshes=hats, evidence=ev, hair_templates=hair_tpls, beards=sorted(set(beards)),
                head_top=round(top, 3) if top is not None else None)


# ------------------------------------------------------------------------------------------------ hairstyles
# 2026-10-11: Character -> Hairstyle -> iterations. A hairstyle = one distinct set of hair meshes worn across the
# character's appearances (its template plus the quest / DLC copies <name>[_dlc|_bob|_ep1].w2ent). The hairstyle of the
# roster appearance is "default" (every existing config, which has no `style` key, belongs to it); the others get an id
# + a human name from the appearances that wear them (STYLE_NAMES overrides). A config opts into a style with the key
# "style": "<id>" (tools/hairworks/chars/<cfg>.json); approved builds per style live in APPROVED.json "approved_styles".
# Catalog contract (desk_prep.py --catalog, the Studio picker): per character `hairstyles` = [{id, name,
# appearance_or_component, thumb_painted, thumb_vanilla, ...}], default first.
STYLE_NAMES = {
    # (character, hair template basename) -> (id, name)
    ('yennefer', 'c_03_wa__yennefer'): ('bath', 'Bath / alternative look'),
    ('keira', 'c_01_wa__keira_bloody'): ('bloody', 'Bloody (q310)'),
    ('anna', 'c_03_wa__anna_henrietta'): ('party', 'Mandragora party'),
    ('avallach', 'c_08_ma__avallach'): ('masked', 'Masked'),
    ('cerys', 'c_16_wa__cerys02'): ('crown', 'Crowned'),
    ('hjalmar', 'c_02_mb__hjalmar'): ('crown', 'Crowned'),
    ('dettlaff', 'c_01_ma__dettlaff_vampire'): ('vampire', 'Vampire'),
    ('eredin', 'c_14_mw__eredin02'): ('helmet', 'Under the helmet'),
    ('iris', 'c_02_wa__iris'): ('veil', 'Veiled'),
}
# templates that are pools of several people (Graden: 17 witch hunters; Johnny's template also holds the godling girl):
# only the hairstyle of the roster appearance is the character's
SINGLE_STYLE = {'graden', 'johnny'}
STYLE_RULES = (     # (substring of the appearance names, id, name): the first rule matching any appearance of the style
    ('hood', 'hooded', 'Hooded'), ('covered', 'hooded', 'Hooded'),
    ('bloody', 'bloody', 'Bloody'), ('travel', 'travel', 'Travel'), ('party', 'party', 'Party'), ('wedding', 'wedding', 'Wedding'),
    ('crown', 'crown', 'Crowned'), ('naked', 'bath', 'Bath / lingerie'), ('lingerie', 'bath', 'Bath / lingerie'),
    ('ball', 'ball', 'Ball gown'), ('dress', 'dress', 'Dress'), ('gown', 'gown', 'Gown'), ('winter', 'winter', 'Winter'),
)


def _bn(p):
    return p.replace('\\', '/').rsplit('/', 1)[-1]


def related_templates(tpl):
    """the character's entity template + its quest / DLC copies with the same base name (triss.w2ent:
    quests\\main_npcs\\triss.w2ent, dlc6 triss_dlc.w2ent, bob triss_bob.w2ent)"""
    import re
    import gamefiles as G
    b = re.escape(_bn(tpl)[:-len('.w2ent')].lower())
    out = [tpl]
    for p in G.find(r'(^|[\\/])%s(_dlc|_bob|_ep1)?\.w2ent$' % b):
        if ('npc_entities' in p or 'npcs' in p) and p.lower() != tpl.lower() and p not in out:
            out.append(p)
    return out


_HAIR_CACHE = {}


def hair_meshes(inc):
    """hair meshes of one included template: pbr_hair card meshes of a c_* hair template (not beards / brows /
    lashes, not fur collars of body templates), or any CFurComponent"""
    if inc not in _HAIR_CACHE:
        import discover as D
        res = []
        for comp, cls, mesh, fm in D.mesh_comps(inc):
            if cls == 'CFurComponent' or (_bn(inc).lower().startswith('c_') and D.classify(mesh) == 'hair'):
                res.append(_bn(mesh))
        _HAIR_CACHE[inc] = res
    return _HAIR_CACHE[inc]


def hairstyles(key, rec):
    """-> [{id, name, appearance_or_component, template, component, meshes, appearances, thumb_painted, thumb_vanilla}]
    one per distinct hair-mesh set across the appearances of the character's templates, default first"""
    import discover as D
    tpl, app = rec.get('template'), rec.get('appearance')
    groups = {}            # sorted mesh tuple -> dict(comps, apps)
    if tpl:
        for t in related_templates(tpl):
            try:
                apps = D.appearances(t)
            except Exception:
                continue
            for a, incs in apps.items():
                comps = [i for i in incs if hair_meshes(i)]
                meshes = tuple(sorted({m for i in comps for m in hair_meshes(i)}))
                if not meshes:
                    continue      # no hair (head towel, bald-cap appearances)
                g = groups.setdefault(meshes, dict(comps=[], apps=[], tpls=[]))
                for i in comps:
                    if i not in g['comps']:
                        g['comps'].append(i)
                if a not in g['apps']:
                    g['apps'].append(a)
                if t not in g['tpls']:
                    g['tpls'].append(t)
    dkey = next((k for k, g in groups.items() if app in g['apps'] and tpl in g['tpls']), None)
    order = sorted(groups, key=lambda k: (k != dkey, -len(groups[k]['apps'])))
    if key in SINGLE_STYLE:
        order = [k for k in order if k == dkey]
    out, used = [], set()
    pdir = os.path.join(os.path.dirname(os.path.dirname(HERE)), 'build', 'desktop_studio', 'portraits_ai')
    for k in order:
        g = groups[k]
        comp = g['comps'][0] if g['comps'] else ''
        if k == dkey:
            sid, name = 'default', 'Default'
        else:
            sid, name = STYLE_NAMES.get((key, _bn(comp)[:-len('.w2ent')]), (None, None))
            if not sid:
                low = [x.lower() for x in g['apps']]
                if all(x.endswith('_dlc') for x in low) or all(_bn(t).lower().endswith('_dlc.w2ent') for t in g['tpls']):
                    sid, name = 'dlc', 'Alternative look (DLC)'
                else:
                    sid, name = next(((i, n) for s, i, n in STYLE_RULES if any(s in x for x in low)), (None, None))
            if not sid:
                sid = _bn(comp)[:-len('.w2ent')].lower() or 'style'
                name = sid.replace('_', ' ')
        base, n = sid, 2
        if sid in used:
            name = '%s %s' % (name, g['apps'][0])
        while sid in used:
            sid, n = '%s%d' % (base, n), n + 1
        used.add(sid)
        rep = app if k == dkey else g['apps'][0]
        painted = os.path.join(pdir, (key if sid == 'default' else '%s__%s' % (key, sid)) + '.jpg')
        out.append(dict(id=sid, name=name, appearance_or_component=rep, template=g['tpls'][0], component=comp,
                        meshes=list(k), appearances=g['apps'],
                        thumb_painted=os.path.relpath(painted, os.path.dirname(os.path.dirname(HERE))).replace('\\', '/')
                        if os.path.exists(painted) else None, thumb_vanilla=None))
    if not out or out[0]['id'] != 'default':
        out.insert(0, dict(id='default', name='Default', appearance_or_component=app, template=tpl, component='',
                           meshes=[], appearances=[app] if app else [], thumb_painted=None, thumb_vanilla=None))
    return out


def update_hairstyles(keys=None):
    """add / refresh `hairstyles` in roster.json in place (re-read right before writing; entries written by other
    tools, e.g. Geralt's, are kept: only characters without hairstyles or with ours (hairstyles_src) are touched)"""
    ro = json.load(open(OUT, encoding='utf-8'))
    res = {}
    for k, c in sorted(ro['characters'].items()):
        if keys and k not in keys:
            continue
        if c.get('hairstyles') and c.get('hairstyles_src') != 'roster.py':
            continue
        res[k] = hairstyles(k, c)
        print('%-13s %s' % (k, ', '.join('%s (%s, %d app)' % (s['name'], s['component'] and _bn(s['component']), len(s['appearances'])) for s in res[k])))
    ro = json.load(open(OUT, encoding='utf-8'))
    for k, hs in res.items():
        c = ro['characters'].get(k)
        if c is None or (c.get('hairstyles') and c.get('hairstyles_src') != 'roster.py'):
            continue
        c['hairstyles'] = hs
        c['hairstyles_src'] = 'roster.py'
    json.dump(ro, open(OUT, 'w', encoding='utf-8'), indent=1, ensure_ascii=False)
    multi = sorted(k for k, c in ro['characters'].items() if len(c.get('hairstyles') or []) > 1)
    print('hairstyles: %d characters updated; more than one style: %s' % (len(res), ', '.join(multi)))


# ------------------------------------------------------------------------------------------------ beards (2026-10-15)
# `beard` per character (additive field, every other field unchanged): {"type": "cards"|"painted"|"none", "mesh",
# "appearance", "evidence", ...}.  cards = an hb_* CMeshComponent with beard cards (convertible like Zoltan's, beard.py);
# painted = no hb_* mesh, the stubble / beard is in the head texture (nothing to convert); none = no beard mesh found
# (women, bald clean-shaven, helmets) or an hb_* mesh that only holds eyebrow cards (Avallac'h, Eredin, Oriana).
# The mesh facts are read from the game files (every appearance of the character's templates); PAINTED lists the
# documented painted-stubble heads (HAIRWORK.md "Beards"), HEAD_HAIR_NOTE the head hair the auto analysis cannot see.
PAINTED = {'eskel': 'HAIRWORK.md Beards: stubble painted in the head texture',
           'emhyr': 'HAIRWORK.md Beards: stubble painted in the head texture',
           'letho': 'HAIRWORK.md Beards: stubble painted in the head texture',
           'imlerith': 'HAIRWORK.md Beards: stubble painted in the head texture',
           'roche': 'no hb_* mesh; stubble painted in the head texture (UNVERIFIED: texture not inspected)'}
HEAD_HAIR_NOTE = {
    'ermion': 'hh_01_ma__mousesack.w2mesh (pbr_hair, z 1.66..1.84, template c_01_ma__mousesack.w2ent) sits UNDER the antler '
              'hat c_01_ma__mousesack (z 1.71..2.18); the roster auto analysis misses it (below head top 1.89). Hair cards '
              'exist, hidden by the hat in the only appearance',
    'baron': 'hh_01_ms__nml_baron.w2mesh (pbr_hair, z 1.76..1.86, 0.10 m tall) = the hood / cap (L235); same in all 3 appearances',
}


def _beard_cards(mesh):
    """-> (beard card components, eyebrow card components, triangles of the beard cards) of a hb_* mesh, LOD0"""
    import numpy as np
    import gamefiles as G
    import beard as B
    m = G.load_mesh(mesh)
    ch = G.merge_chunks(m, m['lod0'])
    lab, cls = B.card_classes(ch, G.mesh_bones(mesh)['names'])
    nb = len(np.unique(lab[cls == 'beard']))
    nr = len(np.unique(lab[cls == 'brow']))
    idx = np.asarray(ch['idx']).reshape(-1, 3)
    tri = int((cls[idx[:, 0]] == 'beard').sum())
    return nb, nr, tri


def beard_entry(key, rec):
    """the `beard` dict of one roster record, from the game files"""
    import discover as D
    per_app, meshes = {}, {}
    for tpl in related_templates(rec['template']):
        try:
            apps = D.appearances(tpl)
        except Exception:
            continue
        for app, incs in apps.items():
            for inc in incs:
                try:
                    for comp, cls, mesh, fm in D.mesh_comps(inc):
                        if D.classify(mesh) == 'beard':
                            per_app.setdefault(app, set()).add(mesh)
                            meshes[mesh] = None
                except Exception:
                    pass
    for mesh in meshes:
        try:
            meshes[mesh] = _beard_cards(mesh)
        except Exception as e:
            meshes[mesh] = 'unreadable (%s)' % type(e).__name__
    def count(m):
        v = meshes.get(m)
        return v[0] if isinstance(v, tuple) else -1
    app = rec['appearance']
    mine = sorted(per_app.get(app, ()), key=lambda m: -count(m))
    out = dict(type='none', mesh=None, appearance=app)
    if mine:
        m = mine[0]
        v = meshes[m]
        out['mesh'] = _bn(m)
        if isinstance(v, tuple) and v[0] > 0:
            out.update(type='cards', cards=v[0], brow_cards=v[1], triangles=v[2])
            out['evidence'] = '%s: %d beard cards (%d tris) + %d eyebrow cards, pbr_hair, appearance %s' % (_bn(m), v[0], v[2], v[1], app)
        elif isinstance(v, tuple):
            out['evidence'] = '%s holds only eyebrow cards (%d), no beard cards' % (_bn(m), v[1])
        else:           # Dettlaff: gamefiles.load_mesh cannot read it (buffer offset); an hb_* card mesh like the others
            out['type'] = 'cards'
            out['evidence'] = '%s: %s; beard mesh present, card count UNVERIFIED' % (_bn(m), v)
    elif key in PAINTED:
        out.update(type='painted', evidence=PAINTED[key])
    else:
        out['evidence'] = 'no hb_* beard mesh in any appearance' if not meshes else 'no hb_* mesh in %s' % app
    other = sorted({_bn(m) for a, ms in per_app.items() if a != app for m in ms} - {_bn(m) for m in mine})
    if other:
        out['other_beard_meshes'] = other
    apps_with = sorted(a for a, ms in per_app.items() if ms)
    if apps_with:
        out['appearances_with_beard'] = apps_with
    if key in HEAD_HAIR_NOTE:
        out['head_hair'] = HEAD_HAIR_NOTE[key]
    return out


def update_beards(keys=None):
    """add / refresh only the `beard` field of roster.json in place (other fields untouched)"""
    ro = json.load(open(OUT, encoding='utf-8'))
    for k, c in sorted(ro['characters'].items()):
        if (keys and k not in keys) or k == 'geralt' or c.get('hair_type') == 'native_hairworks':
            continue
        c['beard'] = beard_entry(k, c)
        b = c['beard']
        print('%-13s %-8s %s' % (k, b['type'], b.get('evidence', '')[:130]))
    json.dump(ro, open(OUT, 'w', encoding='utf-8'), indent=1, ensure_ascii=False)


# ------------------------------------------------------------------------------------------------ Geralt (2026-10-11)
# Geralt's hair is native HairWorks and comes from a `hair` ITEM, not an appearance: the barber items of
# gameplay\items\def_item_hairs.xml (base), dlc\dlc2\...\dlc2_items.xml (free DLC "Beard and Hairstyle Set"),
# gameplay\items\w3r_items.xml (5.0 "Rabble rouser Hair") and dlc\bob\...\def_item_hairs.xml (Fancy Bowl). An item mounts
# one coif template items\bodyparts\geralt_items\coif\<equip_template>.w2ent picked by the armour worn (variants: `b` =
# no armour / naked torso, the catch-all `category="armor"` one = any armour, _br / _inq / _mask / Lynx 4 = special
# collars). Hair items are NoShow (no item name string): the in-game names are the barber dialogue choices, matched
# to the items through the barber scenes (dlc2 shop_*_barber_01.w2scene, bob shop_44_city_south_barber.w2scene: each
# choice line sits next to the item name it gives). "Preview Hair" = Half With Tail (same c_01 coif, UI preview only).
# Not hairstyles: c_01_mg__witcher_mask (masks), c_08_mg__witcher_lx (Lynx armour 4 helmet), the HoS head-cap coif
# dlc\ep1\...\c_08_mg__witcher.w2ent (no item; quest only). There is no "Hen Gaidth" (an armour set), "Sweptback" or
# "Elegant" hairstyle in the 5.0 strings. Capture: geralt_npc.w2ent (no hair of its own) + the item mounted by the test
# script (opt-in config key "hair_item", hairTest.ws HairTestMountHair) after the torso item "Body torso medalion"
# ("hair_torso": the shirtless player, listed in the `b` variant of every hair item). Without it the NPC's "Body torso 01"
# matches no variant and gets the item's DEFAULT equip_template: Long Loose's default c_03b_mg__witcher_m does not
# exist in the game files -> no hair (L238 shot him bald).
GERALT_TPL = E + 'main_npc\\geralt_npc.w2ent'
GERALT_STYLES = [
    # id, short name, HairLab label, hair item, barber line (string id, en text), coif no-armour / with-armour, part, source
    ('default', 'Half with tail (default)', 'geralthalftail', 'Half With Tail Hairstyle',
     (1092736, 'Loosely gathered, tied behind the head.'), 'c_01b_mg__witcher', 'c_01_mg__witcher', 'base', 'barber (base game)'),
    ('shaved_with_tail', 'Shaved with tail', 'geraltshavedtail', 'Shaved With Tail Hairstyle',
     (1092737, 'Shaved except for a pony tail.'), 'c_02b_mg__witcher', 'c_02_mg__witcher', 'base', 'barber (base game)'),
    ('long_loose', 'Long loose', 'geraltlongloose', 'Long Loose Hairstyle',
     (1092738, 'Long and loose.'), 'c_03b_mg__witcher', 'c_03_mg__witcher_m', 'base', 'barber (base game)'),
    ('short_loose', 'Short loose', 'geraltshortloose', 'Short Loose Hairstyle',
     (1092739, 'Loose, not too long.'), 'c_05b_mg__witcher', 'c_05_mg__witcher', 'base',
     'barber (free DLC Beard and Hairstyle Set)'),
    ('mohawk_with_ponytail', 'Mohawk with ponytail', 'geraltmohawk', 'Mohawk With Ponytail Hairstyle',
     (1092740, 'Shaved on the sides, short on top, and a pony tail.'), 'c_06b_mg__witcher', 'c_06_mg__witcher', 'base',
     'barber (free DLC Beard and Hairstyle Set)'),
    ('elven_rebel', 'Elven rebel (Nilfgaardian)', 'geraltelvenrebel', 'Nilfgaardian Hairstyle',
     (1092741, 'The elven rebel cut.'), 'c_07b_mg__witcher', 'c_07_mg__witcher', 'base',
     'barber (free DLC Beard and Hairstyle Set)'),
    ('wild_one', 'Wild One (Rabble-rouser)', 'geraltwildone', 'Rabble rouser Hair',
     (1479191, 'Rabble-rouser hair.'), 'haircut_w3r_troublemaker_01', 'haircut_w3r_troublemaker_01', 'base',
     'barber, unlocked by the 5.0 reward "Wild One" (string 1140041)'),
    ('queens_pageboy', "Queen's pageboy (Fancy Bowl)", 'geraltpageboy', 'Fancy Bowl',
     (1211183, "Queen's pageboy."), 'c_17_mg__witcher_bowl', 'c_17_mg__witcher_bowl', 'baw',
     'barber of Beauclair city south only (Blood and Wine)'),
]


def _coif(name):
    import gamefiles as G
    import re
    hits = G.find(r'geralt_items\\coif\\%s\.w2ent$' % re.escape(name))
    return hits[0] if hits else None


def geralt_hairstyles():
    """Geralt's hairstyles in the catalog shape ({id, name, appearance_or_component, thumb_painted, thumb_vanilla} +
    item / barber line / coif templates / components / furs), default first. Read from the game files."""
    import discover as D
    root = os.path.dirname(os.path.dirname(HERE))
    pdir = os.path.join(root, 'build', 'desktop_studio', 'portraits_ai')
    rel = lambda fp: os.path.relpath(fp, root).replace('\\', '/') if os.path.exists(fp) else None
    out = []
    for sid, name, label, item, (lid, line), naked, armour, part, src in GERALT_STYLES:
        tpl, tpl_a = _coif(naked), _coif(armour)
        comps = [dict(comp=c, cls=cls, mesh=m, fur=f) for c, cls, m, f in D.mesh_comps(tpl)] if tpl else []
        furs = [c['fur'] for c in comps if c['fur']]
        pkey = 'geralt' if sid == 'default' else 'geralt__' + sid
        out.append(dict(id=sid, name=name, appearance_or_component=_bn(tpl or naked)[:-len('.w2ent')] if tpl else naked,
                        item=item, display_name=line, display_string_id=lid, template=tpl, template_armour=tpl_a,
                        component=comps[0]['comp'] if comps else '', components=comps,
                        meshes=[_bn(c['mesh']) for c in comps], furs=furs, redfur=bool(furs),
                        hair_kind='native_hairworks' if furs else 'cards', part=part, source=src,
                        hairlab_label=label, baseline=label + '_baseline',
                        portrait_key='geralt__' + ('half_with_tail' if sid == 'default' else sid),
                        thumb_painted=rel(os.path.join(pdir, pkey + '.jpg')),
                        thumb_vanilla=rel(os.path.join(pdir, 'src', 'geralt__%s.png' % (
                            'half_with_tail' if sid == 'default' else sid)))))
    return out


def geralt_entry():
    hs = geralt_hairstyles()
    d = hs[0]
    return dict(name='Geralt of Rivia', part='base', female=False, status='not_started', template=GERALT_TPL,
                appearance='geralt_npc', alt_appearances=[], hair_type='native_hairworks', hair_auto='native_hairworks',
                headgear=False, hair_templates=[], beards=[],
                evidence=['fur %s' % _bn(f) for f in d['furs']], portrait=d['baseline'],
                note='the player; hair = a barber `hair` item (one coif template per armour class), native HairWorks '
                     'except Wild One / Queen\'s pageboy (cards). Captured as geralt_npc.w2ent (forced appearance '
                     'geralt_npc, clean-shaven, no hair of its own) with the item mounted by the test script '
                     '(chars/geralt<style>_baseline.json "hair_item"). Beards out of scope.',
                hairstyles=hs, hairstyles_src='roster.py:geralt', style_baselines=True)


def update_geralt():
    """roster.json: (re)write the `geralt` entry in place (re-read right before writing; nothing else touched)"""
    rec = geralt_entry()
    for s in rec['hairstyles']:
        print('%-22s %-32s %-28s redfur %-5s %s' % (s['id'], s['item'], s['appearance_or_component'], s['redfur'],
                                                   ', '.join(_bn(f) for f in s['furs']) or ', '.join(s['meshes'])))
    ro = json.load(open(OUT, encoding='utf-8'))
    ro['characters']['geralt'] = rec
    ro.get('excluded', {}).pop('geralt', None)
    json.dump(ro, open(OUT, 'w', encoding='utf-8'), indent=1, ensure_ascii=False)
    print('roster.json: geralt, %d hairstyles' % len(rec['hairstyles']))


def write_geralt_baselines():
    """chars/<label>_baseline.json per Geralt hairstyle (never overwrites): geralt_npc.w2ent + the hair item. `mod` =
    modGeraltHairEarCollision (our own Geralt fur collision fix, parked by HairLab for the vanilla shots, restored)."""
    for s in geralt_hairstyles():
        p = os.path.join(HERE, 'chars', s['baseline'] + '.json')
        if os.path.exists(p):
            print('keep  %s' % os.path.relpath(p))
            continue
        cfg = dict(template=GERALT_TPL, appearance='', hair_item=s['item'], hair_torso='Body torso medalion',
                   mod='modGeraltHairEarCollision',
                   captures='analysis\\hairworks_geralt\\captures', hair=[],
                   note='roster portrait baseline: vanilla Geralt, hairstyle %s (%s, coif %s); geralt_npc.w2ent + hair item '
                        'mounted by the test script (opt-in "hair_item"); 2026-10-11, roster.py --geralt-baselines'
                        % (s['name'], s['item'], s['appearance_or_component']),
                   vanilla_recapture=True, calm_ai=True)
        json.dump(cfg, open(p, 'w'), indent=2)
        print('wrote %s' % os.path.relpath(p))


def existing_entry(key):
    p = os.path.join(HERE, 'chars', key + '_baseline.json')
    C = json.load(open(p)) if os.path.exists(p) else {}
    name, part, sex = EXISTING[key]
    return dict(name=name, tpl=C.get('template'), app=C.get('appearance'), part=part, sex=sex)


def main(check=False):
    out = dict(note='generated by tools/hairworks/roster.py (2026-10-11); hair_type = curated verdict, auto = mesh analysis',
               excluded=EXCLUDED, characters={})
    for key in sorted((set(EXISTING) | set(ROSTER)) - set(EXCLUDED)):
        r = existing_entry(key) if key in EXISTING else dict(ROSTER[key])
        a = analyse(r['tpl'], r['app'])
        ht = r.get('hair_type') or a['auto']
        if ht == 'cards' and a['headgear'] and key not in EXISTING:
            ht = 'cards'                  # hat over cards: still convertible (Dandelion / Priscilla)
        rec = dict(name=r['name'], part=r['part'], female=r['sex'] == 'f',
                   status='existing' if key in EXISTING else 'not_started',
                   template=r['tpl'], appearance=r['app'], alt_appearances=r.get('alt', []),
                   hair_type=ht, hair_auto=a['auto'], headgear=a['headgear'],
                   hair_templates=a['hair_templates'], beards=a['beards'], evidence=a['evidence'],
                   portrait=None if r.get('no_portrait') else
                   key + '_baseline' if key not in ('margarita',) else 'margaritahealed_baseline')
        if r.get('note'):
            rec['note'] = r['note']
        if 'hairstyles' not in rec:
            rec['hairstyles'] = hairstyles(key, rec)
            rec['hairstyles_src'] = 'roster.py'
        rec['beard'] = beard_entry(key, rec)
        out['characters'][key] = rec
        print('%-13s %-17s auto %-12s %-5s %s' % (key, ht, a['auto'], 'hat' if a['headgear'] else '', '; '.join(a['evidence'])[:150]))
        if check:
            continue
    out['characters']['geralt'] = geralt_entry()       # item-based hairstyles, no appearance analysis (2026-10-11)
    if not check:
        json.dump(out, open(OUT, 'w', encoding='utf-8'), indent=1, ensure_ascii=False)
        from collections import Counter
        cnt = Counter(c['hair_type'] for c in out['characters'].values())
        new = Counter(c['hair_type'] for c in out['characters'].values() if c['status'] == 'not_started')
        print('roster: %d characters (%d new) -> %s' % (len(out['characters']), sum(new.values()), os.path.relpath(OUT)))
        print('  all: %s' % dict(cnt))
        print('  new: %s' % dict(new))


def write_baselines(keys=None):
    """chars/<key>_baseline.json (HairLab vanilla re-capture config) for every not_started roster character that has
    none yet (never overwrites). `mod` names a mod that does not exist (nothing of ours to park); `hair` = the card
    templates of the default appearance (the magenta twin tints them; empty for bald / beard-only heads: no twin)."""
    ro = json.load(open(OUT, encoding='utf-8'))['characters']
    for k, c in sorted(ro.items()):
        if c['status'] != 'not_started' or (keys and k not in keys) or k in EXCLUDED or c.get('portrait') is None \
                or c.get('style_baselines'):     # Geralt: one baseline per hairstyle (--geralt-baselines)
            continue                     # no_portrait: nothing to capture (Johnny: user screenshot)
        p = os.path.join(HERE, 'chars', k + '_baseline.json')
        if os.path.exists(p):
            print('keep  %s' % os.path.relpath(p))
            continue
        cfg = dict(template=c['template'], appearance=c['appearance'], mod='mod%sHairworks' % k.capitalize(),
                   captures='analysis\\hairworks_%s\\captures' % k,
                   hair=[dict(template=t) for t in c['hair_templates']],
                   note='roster portrait baseline (vanilla re-capture of %s, appearance %s, hair type %s); 2026-10-11, '
                        'tools/hairworks/roster.py --baselines' % (c['name'], c['appearance'], c['hair_type']),
                   vanilla_recapture=True, calm_ai=True)
        json.dump(cfg, open(p, 'w'), indent=2)
        print('wrote %s' % os.path.relpath(p))


if __name__ == '__main__':
    if '--geralt' in sys.argv:
        update_geralt()
    elif '--geralt-baselines' in sys.argv:
        write_geralt_baselines()
    elif '--hairstyles' in sys.argv:
        i = sys.argv.index('--hairstyles')
        update_hairstyles(set(sys.argv[i + 1].split(',')) if len(sys.argv) > i + 1 else None)
    elif '--beards' in sys.argv:
        i = sys.argv.index('--beards')
        update_beards(set(sys.argv[i + 1].split(',')) if len(sys.argv) > i + 1 else None)
    elif '--baselines' in sys.argv:
        i = sys.argv.index('--baselines')
        write_baselines(set(sys.argv[i + 1].split(',')) if len(sys.argv) > i + 1 else None)
    else:
        main('--check' in sys.argv)
