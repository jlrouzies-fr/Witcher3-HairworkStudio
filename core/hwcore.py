r"""hwcore - shared facts about the "Hairwork for All - Core" dependency mod (read by the Core build, the pipeline's
release mode, the Desktop Studio export and HairLab).

The Core is ONE mod folder, mod0000HairworkForAllCore, that release builds of our character mods require. The Core (1.0.0, first release)
(2026-10-11) is is a SCRIPT dependency: the Core defines global WitcherScript functions
(content\scripts\local\hairworkforall_core.ws), and every release mod ships one small script that calls them
(content\scripts\local\hwfa_req_<id>.ws). Without the Core (or with a Core older than the mod needs) the game's script
compiler fails with "function ... not found", naming the missing function, so the game does not start until the Core is
installed. Release mods are self-contained otherwise (their textures are the same as a dev export's). See core/README.md.

Versions: the Core defines HairworkForAllCore_v<major>_<minor>() for every minor of its major it is compatible with
(SCRIPT_LEVELS); a release mod calls the one of the version it needs (required_version), so a mod that needs a newer
Core fails to compile on an older one. An early unpublished texture-only Core (no script) is superseded; it is detected by its missing script.
"""
import json
import os
import re
import zlib

CORE_VERSION = '1.0.0'
CORE_MOD = 'mod0000HairworkForAllCore'          # mod0000 sorts before every mod000_ / modA..Z folder: loads first
CORE_NAME = 'Hairwork for All – Core'
NEXUS_ID = 13996
NEXUS_URL = 'https://www.nexusmods.com/witcher3/mods/%d' % NEXUS_ID
MARKER = 'version.json'                         # next to content\ in the Core folder (Studio / tools read it)
SCRIPT_REL = 'content\\scripts\\local\\hairworkforall_core.ws'   # the Core's script (inside the mod folder)
REQUIRED_FN = 'HairworkForAllCore_Required_GetItFromNexus%d' % NEXUS_ID
VERSION_FN = 'HairworkForAllCore_Version'
# major.minor levels the current Core provides (one function each). Add the new minor here when a minor ships;
# drop a level only on a major bump.
SCRIPT_LEVELS = ['1.0']
RELEASE_REQUIRES = '1.0.0'                      # Core version a release mod built today needs (first release)
# Second release requirement (2026-10-11, L244): with path-traced hair the game's default root alpha mode 1 (stochastic
# root fade) leaves see-through gaps (Guillaume's jaw window, Sheala's cheek); Graphics Control's mode 0 closes them.
# Release-only text (REQUIREMENTS.txt / hairworkforall_mod.json / INSTALL_INSTRUCTIONS.txt): bundles and furs are not
# affected. The Graphics Control Nexus URL is not referenced in the repo or the installed mod: placeholder until known.
GC_NAME = 'Graphics Control'
GC_NEXUS_URL = 'https://www.nexusmods.com/witcher3/mods/13349'
GC_SETTING = "PT hair > 'Hair root transparency mode' = 0"
# L249gc2 / L250gc3: the menu option writes [GC_Store] PTHairRootAlphaMode; GC derives its GCP_ option from that store
# value, so a hand-written GCP_PTHairRootAlphaMode=0 line alone is ignored (the live value stays 1)
GC_INI = '[GC_Store] PTHairRootAlphaMode=0 in dx12user.settings'

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
GAME = os.environ.get("HWFA_GAME") or os.path.dirname(ROOT)   # HWFA_GAME: game folder of a release install

# the early unpublished texture Core shipped copies of the vanilla hair-card atlases under this prefix; a fur still pointing there (a release build
# made with the early texture tooling) is mapped back to the game's own texture by legacy_vanilla.
LEGACY_PREFIX = 'dlc\\hairworkforall\\core\\textures\\hair\\'
_VAN = 'characters\\models\\common\\textures\\hair\\'


def legacy_vanilla(path):
    """game texture path of an early-Core texture path (or None)."""
    p = str(path)
    if p.lower().startswith(LEGACY_PREFIX.lower()):
        return _VAN + p[len(LEGACY_PREFIX):]
    return None


def vkey(v):
    return tuple(int(x) for x in re.findall(r'\d+', str(v))[:3])


def level_fn(version):
    """HairworkForAllCore_v<major>_<minor> of a version string."""
    k = (vkey(version) + (0, 0))[:2]
    return 'HairworkForAllCore_v%d_%d' % k


def required_version(*_ignored):
    """Core version a release mod needs (the script API it calls)."""
    return RELEASE_REQUIRES


def version_doc():
    return {'name': CORE_NAME, 'mod': CORE_MOD, 'version': CORE_VERSION, 'nexus': NEXUS_URL,
            'script': SCRIPT_REL, 'functions': [REQUIRED_FN, VERSION_FN] + [level_fn(lv) for lv in SCRIPT_LEVELS]}


def core_script():
    """content of the Core's hairworkforall_core.ws (ASCII, CRLF)."""
    lines = [
        '// Hairwork for All - Core v%s (%s)' % (CORE_VERSION, NEXUS_URL),
        '// Release hair mods made with Hairwork Studio call these functions, so they need this mod installed.',
        '// Licence: LICENSE.md in this folder. Do not copy these functions into another mod.',
        '',
        'function %s() : bool { return true; }' % REQUIRED_FN,
        '',
        'function %s() : string { return "%s"; }' % (VERSION_FN, CORE_VERSION),
        '',
        '// one function per compatible major.minor: a mod that needs a newer Core fails to compile on this one',
    ]
    lines += ['function %s() : bool { return true; }' % level_fn(lv) for lv in SCRIPT_LEVELS]
    return ('\r\n'.join(lines) + '\r\n').encode('ascii')


MOD_NAME_RE = re.compile(r'^mod[A-Za-z0-9_]+$')


def release_id(mod_name, *salt):
    """unique WitcherScript identifier part of a release mod: <mod folder name>_<crc32 of mod + salt, 8 hex>.
    The mod folder name is already unique among installed mods (letters, digits, _ only = a valid identifier tail);
    the hash also keeps two different exports that reuse one folder name (other config / tag) apart."""
    if not MOD_NAME_RE.match(mod_name or ''):
        raise SystemExit('release mod folder name must start with "mod" and use letters, digits and _ only: %r' % mod_name)
    h = zlib.crc32('|'.join([mod_name] + [str(s) for s in salt if s]).encode('utf-8')) & 0xFFFFFFFF
    return '%s_%08x' % (mod_name, h)


def release_script(mod_name, version, *salt):
    """-> (relative path inside the mod folder, bytes) of the release mod's requirement script."""
    rid = release_id(mod_name, *salt)
    fn = 'HWFA_Check_' + rid
    lines = [
        '// %s requires %s v%s or newer: %s' % (mod_name, CORE_NAME.replace('–', '-'), version, NEXUS_URL),
        '// If the game reports "%s" or "%s" as not found, install / update that mod.' % (REQUIRED_FN, level_fn(version)),
        'function %s() : bool' % fn,
        '{',
        '    return %s() && %s();' % (REQUIRED_FN, level_fn(version)),
        '}',
    ]
    rel = 'content\\scripts\\local\\hwfa_req_%s.ws' % rid
    return rel, ('\r\n'.join(lines) + '\r\n').encode('ascii')


def write_release_script(mod_dir, mod_name, version, *salt):
    rel, data = release_script(mod_name, version, *salt)
    p = os.path.join(mod_dir, rel)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, 'wb') as fh:
        fh.write(data)
    return rel


def installed(game=GAME):
    """-> (installed?, version or None) of the Core in <game>\\mods. A Core counts only when its script is there;
    a folder without it (the early texture Core) reports no version = incomplete."""
    d = os.path.join(game, 'mods', CORE_MOD)
    if not os.path.isdir(d):
        return False, None
    if not os.path.isfile(os.path.join(d, SCRIPT_REL)):
        return True, None
    try:
        return True, json.load(open(os.path.join(d, MARKER), encoding='utf-8')).get('version')
    except Exception:
        return True, None


def requirement_line(version, ascii=False):
    """ascii=True for console logs and cp1252 text files (en dash -> hyphen)."""
    return 'Requires %s v%s or newer: %s' % (CORE_NAME.replace('–', '-') if ascii else CORE_NAME, version, NEXUS_URL)


def gc_requirement_line(ascii=False):
    """the Graphics Control requirement of release mods (path-traced hair root transparency mode 0)."""
    return ("Requires %s (Nexus) %s set PT hair > 'Hair root transparency mode' to 0; otherwise path-traced hair "
            "shows see-through gaps: %s" % (GC_NAME, '-' if ascii else '—', GC_NEXUS_URL))


def write_release_files(mod_dir, mod_name, version, extra=None, salt=()):
    """release mod folder (next to content\\): the requirement script (content\\scripts\\local\\hwfa_req_<id>.ws),
    REQUIREMENTS.txt and hairworkforall_mod.json (requires_core)."""
    rel = write_release_script(mod_dir, mod_name, version, *salt)
    with open(os.path.join(mod_dir, 'REQUIREMENTS.txt'), 'w', encoding='utf-8') as fh:
        fh.write(requirement_line(version) + '\n\n'
                 'Install the Core mod folder (%s) into your Witcher 3 "mods" folder next to %s.\n'
                 'Without the Core the game will refuse to start: its script compiler reports\n'
                 '"%s" (or "%s") as missing.\n\n' % (CORE_MOD, mod_name, REQUIRED_FN, level_fn(version))
                 + gc_requirement_line() + '\n'
                 'Pick it in the game menu (Options > Mods > Graphics Control, path-traced hair group):\n'
                 '"Hair root transparency mode" = "Mode 0".\n'
                 '(Graphics Control stores it as %s; editing only the GCP_PTHairRootAlphaMode\n'
                 'line by hand does not work, Graphics Control keeps applying mode 1).\n' % GC_INI)
    meta = {'mod': mod_name, 'requires_core': version, 'core_mod': CORE_MOD, 'core_nexus': NEXUS_URL,
            'core_check': rel.replace('\\', '/'), 'core_functions': [REQUIRED_FN, level_fn(version)],
            'requires': [{'name': CORE_NAME, 'nexus': NEXUS_URL, 'version': version, 'mod': CORE_MOD},
                         {'name': GC_NAME, 'nexus': GC_NEXUS_URL, 'setting': GC_SETTING, 'ini': GC_INI,
                          'why': 'otherwise path-traced hair shows see-through gaps'}]}
    meta.update(extra or {})
    with open(os.path.join(mod_dir, 'hairworkforall_mod.json'), 'w', encoding='utf-8') as fh:
        json.dump(meta, fh, indent=2)
        fh.write('\n')
    return meta
