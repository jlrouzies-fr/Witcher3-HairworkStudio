"""Static consistency checks for a patched hair entity template (outer + flatCompiledData)."""
import sys, struct
import os
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from cr2w_reader import CR2WFile, Ref
from w3lib import CR2W
from cr2w_edit import CR2WDoc, Chunk, read_ptr_lists, entity_components


def check_file(f, label, problems):
    n = len(f.exports)
    for e in f.exports:
        if e.error: problems.append(f'{label} export {e.i} {e.cls}: {e.error}')
        for nm in e.order:
            typ, v = e.props[nm]
            items = v[2] if isinstance(v, tuple) and v[0] == 'array' else [v]
            for x in items:
                if isinstance(x, Ref):
                    if x.kind == 'export' and not (0 <= x.idx < n): problems.append(f'{label} {e.cls}.{nm} bad export ref {x.idx}')
                    if x.kind == 'import' and not (0 <= x.idx < len(f.imports)): problems.append(f'{label} {e.cls}.{nm} bad import ref {x.idx}')
    # entity component list + attachment lists
    doc = CR2WDoc.parse(f.d)
    ent_i = next(i for i, e in enumerate(doc.exports) if e.cls in ('CEntity', 'CItemEntity'))
    _h, ptrs, rest = entity_components(Chunk(doc, ent_i).trailing)
    comps = [i for i, e in enumerate(doc.exports) if e.cls.endswith('Component')]
    if sorted(p - 1 for p in ptrs) != comps: problems.append(f'{label} entity component list {ptrs} != components {comps}')
    for i, e in enumerate(doc.exports):
        c = Chunk(doc, i)
        if e.cls.endswith('Component') and c.trailing:
            lists, r = read_ptr_lists(c.trailing, 2)
            if r: problems.append(f'{label} {e.cls}#{i} trailing rest {r.hex()}')
        if e.cls == 'CMeshSkinningAttachment':
            par = struct.unpack('<i', c.get('parent').value)[0] - 1
            ch = struct.unpack('<i', c.get('child').value)[0] - 1
            pl, _ = read_ptr_lists(Chunk(doc, par).trailing, 2)
            cl, _ = read_ptr_lists(Chunk(doc, ch).trailing, 2)
            if i + 1 not in pl[1]: problems.append(f'{label} att#{i} not in parent {par} child-list {pl}')
            if i + 1 not in cl[0]: problems.append(f'{label} att#{i} not in child {ch} parent-list {cl}')
            tp = struct.unpack('<i', Chunk(doc, ch).get('transformParent').value)[0] - 1
            if tp != i: problems.append(f'{label} att#{i} child transformParent {tp}')
    return doc


def validate(data, label='file'):
    problems = []
    f = CR2WFile(data)
    bad, ok = CR2W(data).verify_crcs()
    if bad or not ok: problems.append(f'{label} crc bad={bad} hdr={ok}')
    d = check_file(f, label, problems)
    flat = [x for _, x in f.nested_cr2w()]
    if len(flat) != 1 or not isinstance(flat[0], CR2WFile):
        problems.append(f'{label} flatCompiledData missing/undecodable')
    else:
        nf = flat[0]
        bad, ok = CR2W(nf.d).verify_crcs()
        if bad or not ok: problems.append(f'{label} flat crc bad={bad} hdr={ok}')
        check_file(nf, label + ':flat', problems)
        outer_cls = [e.cls for e in f.exports[1:]]
        flat_cls = [e.cls for e in nf.exports]
        if outer_cls != flat_cls: problems.append(f'{label} outer/flat class order differs {outer_cls} vs {flat_cls}')
        if [i['path'] for i in f.imports] != [i['path'] for i in nf.imports]: problems.append(f'{label} outer/flat imports differ')
    return problems, f


if __name__ == '__main__':
    for fn in sys.argv[1:]:
        pr, f = validate(open(fn, 'rb').read(), fn)
        print(fn, 'OK' if not pr else 'PROBLEMS', f.summary())
        for p in pr: print('  ', p)
