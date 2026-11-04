"""Refresh the material presets list inside existing Studio packs (desk.json 'materials').

desk_prep bakes desk_prep.PRESETS into each pack when the pack is built. Packs built before a preset change keep the old
list, e.g. those built before 2026-10-18 13:06 have no "PT ... (optimised)" presets. This rewrites only the
'materials' key and leaves everything else as it is. A running Studio picks the new list up when it reopens the pack.
  py desktop_studio\\tools\\refresh_pack_materials.py [--dry] [root ...]   (default root: build\\desktop_studio)
"""
import json, os, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
import desk_prep  # noqa: E402


def main():
    dry = '--dry' in sys.argv
    roots = [a for a in sys.argv[1:] if not a.startswith('--')] or [os.path.join(REPO, 'build', 'desktop_studio')]
    mats = desk_prep.materials(log=lambda *a: None)
    names = set(mats)
    done = same = skipped = 0
    for root in roots:
        for dp, _dn, fn in os.walk(root):
            if 'desk.json' not in fn:
                continue
            p = os.path.join(dp, 'desk.json')
            if time.time() - os.path.getmtime(p) < 120:   # being written by a session right now
                skipped += 1
                continue
            try:
                with open(p, encoding='utf-8') as fh:
                    d = json.load(fh)
            except Exception:
                skipped += 1
                continue
            if set((d.get('materials') or {})) == names:
                same += 1
                continue
            done += 1
            if dry:
                continue
            d['materials'] = mats
            tmp = p + '.tmp'
            with open(tmp, 'w') as fh:
                json.dump(d, fh, separators=(',', ':'))
            os.replace(tmp, p)
    print('refresh_pack_materials: %s %d packs, %d already current, %d skipped (%d presets)' % (
        'would update' if dry else 'updated', done, same, skipped, len(names)))


if __name__ == '__main__':
    main()
