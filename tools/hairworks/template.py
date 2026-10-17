r"""template - patch a 5.0 hair entity template and make accessory subset meshes.

  patch(data, comp, fur_path, extra=[(new_comp_name, mesh_path), ...]) -> new w2ent bytes
     1. for every `extra`: a new CMeshComponent drawing `mesh_path`, skinned exactly like `comp` (same attachment
        parent = the dyng / animated component) -> accessories keep rendering;
     2. the card CMeshComponent `comp` becomes a CFurComponent IN PLACE (+ furMesh import, explicit drawableFlags).
     Both the outer export table and the nested flatCompiledData are edited in step (the engine instantiates the
     flat copy; validate_ent checks they agree).
  Why in place: a CFurComponent's `mesh` is only drawn as the fallback when HairWorks is off, so the cards vanish
  with HairWorks on (good) - and so does every non-hair island baked into the card mesh (ties, beads, clasps):
  those need the subset meshes below.

  subset_mesh(mesh_rel, keep_mask_fn) -> (w2mesh bytes, buffer bytes, stats)
     copy of a card mesh that draws only the selected islands: every other triangle is collapsed onto its first
     vertex (zero area). The CR2W layout is untouched (only the buffer CRC changes); ship <new>.w2mesh + <new>.w2mesh.1.buffer
     (the buffer goes into a SEPARATE legacy bundle named buffers0.bundle, as the game keeps buffers apart).
"""
import os
import struct
import sys
import uuid
import zlib

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.dirname(HERE))
import w3lib
from cr2w_edit import *
import gamefiles as G


def _find_mesh(doc, comp_name):
    for i, e in enumerate(doc.exports):
        if e.cls in ('CMeshComponent', 'CFurComponent'):
            nm = Chunk(doc, i).get('name')
            if nm and read_string(nm.value) == comp_name:
                return i
    raise KeyError(comp_name)


def mesh_components(data):
    """[(component name, class, mesh path)] of an entity template (outer exports)."""
    from cr2w_reader import CR2WFile
    out = []
    f = CR2WFile(data)
    for e in f.tree():
        if isinstance(e, dict) and e.get('@class') in ('CMeshComponent', 'CFurComponent'):
            m = e.get('mesh') or {}
            out.append((e.get('name'), e['@class'], m.get('@import') if isinstance(m, dict) else None))
    return out


def _swap(doc, comp_name, fur_path, fallback_mesh=None):
    idx = _find_mesh(doc, comp_name)
    imp = doc.add_import(fur_path, 'CFurMeshResource')
    c = Chunk(doc, idx)
    if fallback_mesh:       # the cards drawn when HairWorks is off (complement of the kept card layers: no doubling)
        c.set('mesh', 'handle:CMesh', v_import(doc.add_import(fallback_mesh, 'CMesh')))
    flags = b''.join(struct.pack('<H', doc.name(n)) for n in ('DF_IsVisible', 'DF_CastShadows')) + b'\0\0'
    c.set('drawableFlags', 'EDrawableFlags', flags, after='name')
    c.set('furMesh', 'handle:CFurMeshResource', v_import(imp))
    c.commit()
    doc.exports[idx].cls = 'CFurComponent'
    for n in ('CFurComponent', 'furMesh', 'handle:CFurMeshResource', 'EDrawableFlags'):
        doc.name(n)
    return idx


def _add_mesh_component(doc, src_comp_name, new_name, mesh_path, gp=(120, 420)):
    flat = doc.exports[0].cls != 'CEntityTemplate'
    ent_i = next(i for i, e in enumerate(doc.exports) if e.cls in ('CEntity', 'CItemEntity'))
    src_i = _find_mesh(doc, src_comp_name)
    src = Chunk(doc, src_i)
    ptrs = read_ptr_lists(src.trailing, 2)[0]
    if not ptrs[0]:
        # 2026-10-06 (Oriana: DLC hair entity whose components have NO parent attachment, they are skinned by bones only):
        # the extra component is a plain clone of the source (new name / guid / mesh), no CMeshSkinningAttachment
        imp = doc.add_import(mesh_path, 'CMesh')
        ent_i = next(i for i, e in enumerate(doc.exports) if e.cls in ('CEntity', 'CItemEntity'))
        props = src.clone_props()
        for p in props:
            if p.name == 'guid':
                p.value = uuid.uuid5(uuid.NAMESPACE_URL, 'hairpipe/' + new_name).bytes_le
            elif p.name == 'name':
                p.value = v_string(new_name)
            elif p.name == 'mesh':
                p.value = v_import(imp)
        props = [p for p in props if p.name not in ('graphPositionX', 'graphPositionY')]
        if flat:
            k = [p.name for p in props].index('name') + 1
            props.insert(k, Prop('graphPositionX', 'Int16', v_i16(gp[0])))
            props.insert(k + 1, Prop('graphPositionY', 'Int16', v_i16(gp[1])))
        comp_i = len(doc.exports)
        doc.add_export('CMeshComponent', ent_i, Chunk.build(doc, props, write_ptr_lists([[], []])))
        ent = Chunk(doc, ent_i)
        head, eptrs, rest = entity_components(ent.trailing)
        eptrs.append(comp_i + 1)
        ent.trailing = set_entity_components(head, eptrs, rest)
        ent.commit()
        return comp_i
    att_old = Chunk(doc, ptrs[0][0] - 1)
    parent_ptr = struct.unpack('<i', att_old.get('parent').value)[0]
    imp = doc.add_import(mesh_path, 'CMesh')
    n = len(doc.exports)
    att_i, comp_i = n, n + 1
    att_props = [Prop('parent', 'ptr:CNode', struct.pack('<i', parent_ptr)), Prop('child', 'ptr:CNode', v_ptr(comp_i))]
    doc.add_export('CMeshSkinningAttachment', parent_ptr - 1, Chunk.build(doc, att_props))
    props = src.clone_props()
    for p in props:
        if p.name == 'transformParent':
            p.value = v_ptr(att_i)
        elif p.name == 'guid':
            p.value = uuid.uuid5(uuid.NAMESPACE_URL, 'hairpipe/' + new_name).bytes_le   # deterministic rebuilds
        elif p.name == 'name':
            p.value = v_string(new_name)
        elif p.name == 'mesh':
            p.value = v_import(imp)
    props = [p for p in props if p.name not in ('graphPositionX', 'graphPositionY')]
    if flat:
        k = [p.name for p in props].index('name') + 1
        props.insert(k, Prop('graphPositionX', 'Int16', v_i16(gp[0])))
        props.insert(k + 1, Prop('graphPositionY', 'Int16', v_i16(gp[1])))
    doc.add_export('CMeshComponent', ent_i, Chunk.build(doc, props, write_ptr_lists([[att_i + 1], []])))
    par = Chunk(doc, parent_ptr - 1)
    lists, rest = read_ptr_lists(par.trailing, 2)
    lists[1].append(att_i + 1)
    par.trailing = write_ptr_lists(lists, rest)
    par.commit()
    ent = Chunk(doc, ent_i)
    head, eptrs, rest = entity_components(ent.trailing)
    eptrs.append(comp_i + 1)
    ent.trailing = set_entity_components(head, eptrs, rest)
    ent.commit()
    return comp_i


# 2026-10-15 opt-in (config key template_outer_only, Geralt Ard Headband / modNewHairstylesAndBeards): that mod's coif
# templates carry a STALE flatCompiledData (vanilla c_04 names) while the outer table holds the real component; patch the
# outer table only and leave the flat data as shipped. Default False = unchanged.
OUTER_ONLY = False
FLAT_COMP = None      # 2026-10-15 opt-in (config template_flat_comp): with OUTER_ONLY, ALSO patch the stale flat data, whose
#   hair component has this (old) name (Geralt Ard headband: c_04_mg__long_fur). The game instantiates items from the flat data.
FLAT_NULL_FURS = ()
DROP_FLAT = False     # 2026-10-15 opt-in (config template_drop_flat): validate accepts the missing flat data


def drop_flat(data):
    """2026-10-15 opt-in (config template_drop_flat): the root CEntityTemplate without flatCompiledData."""
    doc = CR2WDoc.parse(data)
    root = Chunk(doc, 0)
    root.remove('flatCompiledData')
    root.commit()
    return doc.to_bytes()


def _edit_both(data, fn):
    doc = CR2WDoc.parse(data)
    # 2026-10-05: third-party mods ship v162 templates (Triss lore hair); cr2w_edit handles 162-164 and keeps the version
    assert doc.version in (162, 163, 164), doc.version
    root = Chunk(doc, 0)
    fl = root.get('flatCompiledData')
    if fl is not None and not OUTER_ONLY:
        cnt, = struct.unpack_from('<I', fl.value, 0)
        flat = CR2WDoc.parse(fl.value[4:4 + cnt])
        fn(flat)
        fb = flat.to_bytes()
        root.set('flatCompiledData', fl.type, struct.pack('<I', len(fb)) + fb)
        root.commit()
    fn(doc)
    return doc.to_bytes()


def _add_script_component(doc, cls, name, gp=(120, 520)):
    """A plain scripted component (no attachment, no mesh) of the WitcherScript class `cls` (opt-in, lore_guard)."""
    flat = doc.exports[0].cls != 'CEntityTemplate'
    ent_i = next(i for i, e in enumerate(doc.exports) if e.cls in ('CEntity', 'CItemEntity'))
    props = [Prop('guid', 'CGUID', uuid.uuid5(uuid.NAMESPACE_URL, 'hairpipe/' + name).bytes_le), Prop('name', 'String', v_string(name))]
    if flat:
        props += [Prop('graphPositionX', 'Int16', v_i16(gp[0])), Prop('graphPositionY', 'Int16', v_i16(gp[1]))]
    comp_i = len(doc.exports)
    doc.add_export(cls, ent_i, Chunk.build(doc, props, write_ptr_lists([[], []])))
    ent = Chunk(doc, ent_i)
    head, eptrs, rest = entity_components(ent.trailing)
    eptrs.append(comp_i + 1)
    ent.trailing = set_entity_components(head, eptrs, rest)
    ent.commit()
    return comp_i


def add_script_component(data, cls, name):
    """w2ent bytes + one scripted component `name` of script class `cls` (outer table and flatCompiledData)."""
    return _edit_both(data, lambda d: _add_script_component(d, cls, name))


def clear_meshes(data, meshes):
    """2026-10-15 opt-in (config key clear_component_meshes): every CMeshComponent drawing one of meshes loses its
    mesh handle (outer table + flatCompiledData); the component stays (no export / attachment change), it draws nothing.
    Used for Triss's i_01 hair clips, which show through the HairWorks."""
    want = {m.lower() for m in meshes}
    hits = []

    def fn(doc):
        for i, e in enumerate(doc.exports):
            if e.cls != 'CMeshComponent':
                continue
            c = Chunk(doc, i)
            m = c.get('mesh')
            if m is None or len(m.value) < 4:
                continue
            h, = struct.unpack_from('<i', m.value, 0)
            if h < 0 and doc.imports[-h - 1][0].lower() in want:
                c.remove('mesh')
                c.commit()
                hits.append(doc.imports[-h - 1][0])
    data = _edit_both(data, fn)
    assert hits, 'clear_component_meshes: none of %s found' % sorted(want)
    return data


def patch(data, comp, fur_path, extra=(), fallback_mesh=None):
    for name, mesh in extra:
        data = _edit_both(data, lambda d, name=name, mesh=mesh: _add_mesh_component(d, comp, name, mesh))
    data = _edit_both(data, lambda d: _swap(d, comp, fur_path, fallback_mesh))
    if OUTER_ONLY and FLAT_COMP:
        doc = CR2WDoc.parse(data)
        root = Chunk(doc, 0)
        fl = root.get('flatCompiledData')
        cnt, = struct.unpack_from('<I', fl.value, 0)
        fb = fl.value[4:4 + cnt]
        for name, mesh in extra:
            fd = CR2WDoc.parse(fb); _add_mesh_component(fd, FLAT_COMP, name, mesh); fb = fd.to_bytes()
        fd = CR2WDoc.parse(fb); _swap(fd, FLAT_COMP, fur_path, fallback_mesh); fb = fd.to_bytes()
        if FLAT_NULL_FURS:
            # opt-in (config template_flat_null_furs): stale extra fur components of the flat data (CDPR's bangs / gap
            # furs) get a null furMesh, so only our fur renders
            fd = CR2WDoc.parse(fb)
            n = 0
            for i in range(len(fd.exports)):
                try:
                    ch = Chunk(fd, i)
                except Exception:
                    continue
                nm, fm = ch.get('name'), ch.get('furMesh')
                if nm is not None and fm is not None and nm.value[1:].rstrip(b'\x00').decode('utf-8', 'replace') in FLAT_NULL_FURS:
                    fm.value = struct.pack('<i', 0); ch.commit(); n += 1
            assert n == len(FLAT_NULL_FURS), (n, FLAT_NULL_FURS)
            fb = fd.to_bytes()
        root.set('flatCompiledData', fl.type, struct.pack('<I', len(fb)) + fb)
        root.commit()
        data = doc.to_bytes()
    doc = CR2WDoc.parse(data)
    root = Chunk(doc, 0)
    sa = root.get('streamedAttachments')
    if sa is not None:
        # streamed attachments record the child's class (dangle -> CMeshComponent "<comp>"): retarget the entries whose
        # childName is the swapped component to CFurComponent (PORT_PLAN variant A; Yennefer c_01 / c_03 have one)
        v = bytearray(sa.value)
        key = struct.pack('<HHI', doc.name('childClass'), doc.name('CName'), 6)
        old = struct.pack('<H', doc.name('CMeshComponent'))
        nkey = struct.pack('<HH', doc.name('childName'), doc.name('CName'))
        cname = struct.pack('<H', doc.name(comp))
        n = 0
        j = v.find(key)
        while j >= 0:
            # the entry's childName is the nearest childName CName before / after this childClass
            a = v.rfind(nkey, 0, j); b = v.find(nkey, j)
            names = [bytes(v[x + 8:x + 10]) for x in (a, b) if x >= 0]
            if v[j + 8:j + 10] == old and (cname in names or not names):
                v[j + 8:j + 10] = struct.pack('<H', doc.name('CFurComponent'))
                n += 1
            j = v.find(key, j + 1)
        sa.value = bytes(v)
        root.commit()
        data = doc.to_bytes()
        print('  streamedAttachments: %d childClass entr%s -> CFurComponent' % (n, 'y' if n == 1 else 'ies'))
    return data


def subset_mesh(mesh_rel, keep_fn, move_fn=None):
    """keep_fn(chunk dict) -> bool vertex mask of what to KEEP (whole components are kept).
    move_fn (2026-10-17 opt-in, card_offset): move_fn(chunk dict) -> new (nv, 3) positions or None; written back into the
    quantised stream A positions of the chunk (None / no move_fn = vertex buffer untouched, byte-identical)."""
    d = bytearray(G.get(mesh_rel))
    buf = bytearray(G.get_buffer(mesh_rel + '.1.buffer'))
    m = G.load_mesh(mesh_rel)
    ibo = m['t']['cookedData']['indexBufferOffset']
    stats = []
    for ci, ch in enumerate(m['chunks']):
        t = m['t']['chunks'][ci]
        sel = keep_fn(ch)
        tri = ch['idx'].reshape(-1, 3).copy()
        if isinstance(sel, tuple) and sel[0] == 'tris':
            # 2026-10-06 (hairline_cards): triangle-level mask, cards cut instead of kept whole
            live = np.asarray(sel[1], bool)
            keep = np.zeros(ch['nv'], bool)
            keep[np.unique(tri[live].reshape(-1))] = True
        else:
            lab = G.components(ch['nv'], ch['idx'])
            keep = np.isin(lab, np.unique(lab[sel]))
            live = keep[tri].all(1)
        tri[~live] = tri[~live][:, :1]
        fi = t.get('firstIndex', 0)
        buf[ibo + fi * 2: ibo + (fi + len(ch['idx'])) * 2] = tri.astype('<u2').tobytes()
        stats.append((int(keep.sum()), int(live.sum()), len(tri)))
        newp = move_fn(ch) if move_fn else None
        if newp is not None:
            cd = m['t']['cookedData']
            sc = np.array([cd['quantizationScale'][k] for k in 'XYZ'])
            of = np.array([cd['quantizationOffset'][k] for k in 'XYZ'])
            q = np.round((np.asarray(newp, float) - of) / sc * 65535.0)
            assert q.min() >= 0 and q.max() <= 65535, 'card_offset moves vertices outside the mesh quantisation box'
            a0 = struct.unpack_from('<4I', ch['rec'], 1)[0]
            for vi in np.nonzero(np.any(np.abs(np.asarray(newp, float) - ch['pos']) > 1e-9, axis=1))[0]:
                struct.pack_into('<3H', buf, a0 + int(vi) * 16, *[int(x) for x in q[vi]])
    t5off, t5cnt, _ = struct.unpack_from('<III', d, 0x28 + 5 * 12)
    assert t5cnt == 1 and struct.unpack_from('<I', d, t5off + 16)[0] == len(buf)
    struct.pack_into('<I', d, t5off + 20, zlib.crc32(bytes(buf)))
    c = w3lib.CR2W(bytes(d))
    c.fix_crcs()
    out = bytes(c.d)
    bad, ok = w3lib.CR2W(out).verify_crcs()
    assert bad == 0 and ok
    return out, bytes(buf), stats


def validate(data, rel, outer_only=()):
    import validate_ent
    pr, _f = validate_ent.validate(data, rel)
    if pr:
        # 2026-10-06 (L128, Crach): a few vanilla entities already have an outer import table longer than the flat one
        # (body_01_mb__crach.w2ent: the outer table also lists the `includes` template). Problems the VANILLA file has
        # itself are not ours: only fail on the new ones. Everything else (crc, class order, attachments) still asserts.
        try:
            vpr, _vf = validate_ent.validate(G.get(rel), rel)
        except Exception:
            vpr = []
        pr = [x for x in pr if x not in vpr]
        if pr and outer_only:
            # 2026-10-15 opt-in (beard.outer_only, Baron): the vanilla head template has outer-only exports (CAnimMimicParam:
            # in the outer export table, never in flatCompiledData). Patching appends components AFTER it, so the vanilla
            # problem string changes; the class-order problem is ours only if the orders still differ without those classes.
            # Only checked when the vanilla file has a class-order problem itself.
            if any('outer/flat class order differs' in x for x in vpr):
                from cr2w_reader import CR2WFile
                f = CR2WFile(data)
                flat = [x for _, x in f.nested_cr2w()]
                if len(flat) == 1 and isinstance(flat[0], CR2WFile):
                    oc = [e.cls for e in f.exports[1:] if e.cls not in outer_only]
                    fc = [e.cls for e in flat[0].exports if e.cls not in outer_only]
                    if oc == fc:
                        pr = [x for x in pr if 'outer/flat class order differs' not in x]
        if pr and OUTER_ONLY and any('outer/flat class order differs' in x for x in vpr):
            # template_outer_only: the source's flat data is stale (class order already differs) and is left as shipped
            pr = [x for x in pr if 'outer/flat class order differs' not in x]
    if OUTER_ONLY and FLAT_COMP:
        # the source's flat data is stale (its own component list never matched the outer one): not ours
        pr = [x for x in pr if 'flat entity component list' not in x]
    if DROP_FLAT:
        pr = [x for x in pr if 'flatCompiledData missing' not in x]
    assert not pr, pr
    bad, ok = w3lib.CR2W(data).verify_crcs()
    assert bad == 0 and ok
