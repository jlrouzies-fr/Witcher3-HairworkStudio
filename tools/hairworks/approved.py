r"""approved - read tools\hairworks\APPROVED.json with hairstyles (2026-10-11).

APPROVED.json keeps its old shape: "approved" maps a character to its approved config (a string = the DEFAULT
hairstyle). Hairstyles (Character -> Hairstyle -> iterations; a config opts into a style with "style": "<id>") add an
optional per-style map, either

  "approved_styles": {"triss": {"dlc": "triss_dlc_l240_a"}}        (preferred: old readers never see it)

or an object instead of the string in "approved" ({"default": "...", "<style>": "..."}). Readers use these helpers so
both forms work:

  default_map(d)   {char: cfg} of the default hairstyle (what every tool read before hairstyles)
  style_map(d)     {char: {style: cfg}} (default included)
  all_cfgs(d)      every approved config of every style (frozen: must rebuild byte-identical)
  approved_for(d, char, style='default')
  style_of(cfg)    the hairstyle of a config ("default" when the key is missing)
"""
import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))
PATH = os.path.join(HERE, 'APPROVED.json')
DEFAULT = 'default'


def load(path=PATH):
    return json.load(open(path, encoding='utf-8'))


def style_map(d=None):
    d = load() if d is None else d
    out = {}
    for ch, v in (d.get('approved') or {}).items():
        if isinstance(v, str):
            out.setdefault(ch, {})[DEFAULT] = v
        elif isinstance(v, dict):
            for st, cfg in v.items():
                if isinstance(cfg, str) and not st.startswith('_'):
                    out.setdefault(ch, {})[st] = cfg
    for ch, v in (d.get('approved_styles') or {}).items():
        if ch.startswith('_') or not isinstance(v, dict):
            continue
        for st, cfg in v.items():
            if isinstance(cfg, str) and not st.startswith('_'):
                out.setdefault(ch, {}).setdefault(st, cfg)
    return out


def default_map(d=None):
    return {ch: m[DEFAULT] for ch, m in style_map(d).items() if DEFAULT in m}


def all_cfgs(d=None):
    return {cfg for m in style_map(d).values() for cfg in m.values()}


def approved_for(d, char, style=DEFAULT):
    return style_map(d).get(char, {}).get(style or DEFAULT)


def style_of(cfg, chars_dir=os.path.join(HERE, 'chars')):
    p = os.path.join(chars_dir, cfg + '.json')
    try:
        return json.load(open(p, encoding='utf-8-sig')).get('style') or DEFAULT
    except Exception:
        return DEFAULT


if __name__ == '__main__':
    for ch, m in sorted(style_map().items()):
        print('%-12s %s' % (ch, ', '.join('%s=%s' % kv for kv in sorted(m.items()))))
