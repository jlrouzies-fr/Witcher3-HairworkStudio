"""Cross-check: C++ cm::merge3 (community_merge_test.exe) vs tools/hairworks/hwgroom.py merge3 on a fixture.
Usage: py crosscheck.py <path to community_merge_test.exe>"""
import json, os, subprocess, sys, tempfile
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', '..', '..', 'tools', 'hairworks'))
import hwgroom


def tr(r, o):
    return {'type': 'translate', 'roots': [r], 'offset': o}


A, B, C, D = [0.0, 0.1, 1.5], [0.01, 0.1, 1.5], [0.02, 0.1, 1.5], [0.03, 0.1, 1.5]
base = [tr(A, [0, 0, 0.001]), {'type': 'delete', 'roots': [D]}]
E, F = [0.04, 0.1, 1.5], [0.05, 0.1, 1.5]
pt = {'type': 'translate', 'roots': [E], 'offset': [0, 0.001, 0], 'falloff': 'point', 'from': 7}
df = {'type': 'delete_face', 'roots': [A, B, F], 'face': 12}
mir = {'type': 'mirror', 'source': '-x', 'plane_x': 0.0012, 'band': 0.015, 'source_roots': [E, F]}
rb = {'type': 'rebuild', 'kind': 'from_cards', 'params': {'spacing_mm': 6, 'where': {'back': True}}, 'studio_version': '0.1.0'}
theirs = base + [tr(B, [0.002, 0, 0]), tr(C, [0, 0.003, 0]), pt, df, mir, rb]
mine = [base[1], tr(C, [0, 0, 0.004]), {'type': 'shorten', 'roots': [A], 'keep': 0.5}, {'type': 'delete_face', 'roots': [B, F, E], 'face': 40}]
p = os.path.join(tempfile.gettempdir(), 'cm_fix.json')
json.dump({'base': base, 'theirs': theirs, 'mine': mine}, open(p, 'w'))
cpp = json.loads(subprocess.check_output([sys.argv[1], p]))
merged, conf = hwgroom.merge3(base, theirs, mine)
c2 = [{'theirs': c['theirs'], 'mine': c['mine'], 'reason': c['reason']} for c in conf]
rt = cpp.get('roundtrip', {})
ok = cpp['merged'] == merged and cpp['conflicts'] == c2 and rt.get('skipped') == 0 and rt.get('equal') is True
print('roundtrip', rt)
print('MATCH' if ok else 'DIFF', len(merged), 'merged ops,', len(conf), 'conflicts')
if not ok:
    print(json.dumps(cpp, indent=1))
    print(json.dumps({'merged': merged, 'conflicts': c2}, indent=1))
sys.exit(0 if ok else 1)
