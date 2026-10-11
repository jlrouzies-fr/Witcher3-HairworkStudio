# Hairwork for All – Core

`mod0000HairworkForAllCore`: the dependency mod that release builds of our hair mods require.
Official download: https://www.nexusmods.com/witcher3/mods/13996 (licence: `core/LICENSE.md`).

Only the **built** Core is published (the `mod0000HairworkForAllCore` folder / its zip). The scripts in this folder stay
in the repository.

## What is in it (v1.0.0)

| File (inside `mod0000HairworkForAllCore\`) | What |
|---|---|
| `content\scripts\local\hairworkforall_core.ws` | the Core's global WitcherScript functions (below) |
| `version.json` | `{"name", "mod", "version", "nexus", "script", "functions"}` (Studio and tools read it) |
| `LICENSE.md` | `core/LICENSE.md` |

```
function HairworkForAllCore_Required_GetItFromNexus13996() : bool { return true; }
function HairworkForAllCore_Version() : string { return "1.0.0"; }
function HairworkForAllCore_v1_0() : bool { return true; }        // one per compatible major.minor (SCRIPT_LEVELS)
```

Only our own files. No game asset (an early unpublished texture-only build shipped copies of the 9 vanilla
hair-card atlases and release mods pointed their textures at them; the script check replaces that), and
release mods use the game's textures again, exactly like dev builds. Zip for Nexus: about 2.3 KB.

**No bundle.** A script-only mod needs no `blob0.bundle` / `metadata.store`: the game compiles
`mods\<mod>\content\scripts\**\*.ws` from any mod folder (checked 2026-10-11 with `witcher3.exe -verifyscriptsandexit`
and an in-game HairLab round with the Core installed, L242). The folder is valid as is.

## How release mods use it

Every release mod ships one script, `content\scripts\local\hwfa_req_<id>.ws`:
```
function HWFA_Check_<id>() : bool
{
    return HairworkForAllCore_Required_GetItFromNexus13996() && HairworkForAllCore_v1_0();
}
```
`<id>` = `<mod folder name>_<crc32 of mod name + config (+ tag), 8 hex>` (`hwcore.release_id`): mod folder names are
letters / digits / `_` after `mod` (checked), so the id is a valid identifier tail, and it is unique per mod folder
(two installed mods cannot share a folder name) and per export. A unique **file name** matters as much as the function
name: the game merges script files by path across mods, so two mods shipping the same `.ws` path would hide one.

Without the Core, the game's script compiler fails and the game will refuse to start. Exact error text
(`witcher3.exe -verifyscriptsandexit`, "Script Compilation Errors" dialog, 2026-10-11):
```
Error [modguillaumehairworkscoretest]local\hwfa_req_modguillaumehairworkscoretest_bb840ddb.ws(5): Could not find function 'HairworkForAllCore_Required_GetItFromNexus13996'
Error [modguillaumehairworkscoretest]local\hwfa_req_modguillaumehairworkscoretest_bb840ddb.ws(5): Could not find function 'HairworkForAllCore_v1_0'
```
(the compiler lower-cases the mod and file names, not the function names). With a Core older than the mod needs, only
the `HairworkForAllCore_v<major>_<minor>` line remains.

- Pipeline: config key `"core": true` (opt-in; `hairpipe.py <cfg> build --release [--mod <testName>]` sets it and builds
  under the config name `<cfg>__release`, so the dev build's editor / preview exports stay untouched). The fur and the
  bundle are the dev build's (checked: Guillaume and Dandelion release bundles identical to `mods\modGuillaumeHairworks`
  / `mods\modDandelionHairworks`); the mod folder gets the requirement script, `REQUIREMENTS.txt` ("Requires Hairwork
  for All – Core v1.0.0 or newer: <link>") and `hairworkforall_mod.json` (`requires_core`, `core_check`,
  `core_functions`). A fur still pointing at an early-Core texture is mapped back to the game's texture.
- Desktop Studio: Export panel, "Export for release (requires Core)" = `desk_export.py --release` (same redfur as the dev
  export plus the same release files and the requirement line in `INSTALL_INSTRUCTIONS.txt`). The dev export ("Export
  edits + mod") is unchanged.
- HairLab: a subject with `"set": {"core": true}` gets its requirement script in the round's batch mod; plan key
  `"core_mod": "install"` installs the built Core for the run (removed after unless it was there before), `"absent"`
  parks an installed Core for the run.
- Second requirement (2026-10-11, L244): `REQUIREMENTS.txt`, `hairworkforall_mod.json` (`requires` list) and the
  Studio's `INSTALL_INSTRUCTIONS.txt` also list "Graphics Control (Nexus) — set PT hair > 'Hair root transparency mode'
  to 0; otherwise path-traced hair shows see-through gaps" (`hwcore.gc_requirement_line`). The Graphics Control Nexus
  URL is a placeholder (`GC_NEXUS_URL = 'https://www.nexusmods.com/witcher3/mods/13349'`) until the user gives it. Text only: bundles
  and furs are unchanged.
- Default builds, dev exports, HairLab rounds without the key and the approved rebuilds are unchanged (byte-identical).

## Build, verify, install, check

```
py core\build_core.py build        # -> build\core\mod0000HairworkForAllCore\ + build\core\HairworkForAllCore_v<ver>.zip, then verify
py core\build_core.py verify       # script = hwcore.core_script(), version.json = CORE_VERSION, nothing else in the folder
py core\build_core.py install      # -> <game>\mods\mod0000HairworkForAllCore (refuses while witcher3.exe runs, checks script path conflicts)
py core\build_core.py uninstall
py core\build_core.py status       # {"installed", "version", "required"}
py core\core_check.py [--core] <release mod folder> ...   # compile oracle (below)
```
Install only through these scripts.

`core_check.py` proves the dependency without a game session: it installs the Core (`--core`) and copies the given
release mod folders into `<game>\mods` under their own (test) names, runs `witcher3.exe -verifyscriptsandexit`
(`compile_oracle.ps1`: a clean compile exits by itself with code 3 in about 10 s; errors open the "Script Compilation
Errors" dialog, read through UI Automation, and the process it started is closed), prints `{"ok", "exit_code",
"errors"}` and removes everything it installed. It refuses while any witcher3.exe runs and never overwrites a mod folder.
Results 2026-10-11: no test mods = exit 3; `modGuillaumeHairworksCoreTest` without the Core = the error above;
Core + that mod = exit 3; Core + `modGuillaumeHairworksCoreTest` + `modDandelionHairworksCoreTest` = exit 3.

## Release on Nexus

1. Bump `CORE_VERSION` in `core/hwcore.py` when the content changes, `py core\build_core.py build`.
2. Upload `build\core\HairworkForAllCore_v<ver>.zip` (mod folder with the script, `version.json` and `LICENSE.md`) as a
   new version of the existing main file on https://www.nexusmods.com/witcher3/mods/13996, with a changelog (never a
   duplicate file).
3. Release character mods after the Core version they need is live.

## Versioning

- `CORE_VERSION` (`core/hwcore.py`) is the single version; the build writes it into `version.json` and the script's
  `HairworkForAllCore_Version()`.
- **Functions are a contract.** A function, once shipped, stays in every later version of the same major.
  A minor adds `HairworkForAllCore_v<major>_<minor>` to `SCRIPT_LEVELS` and keeps the older levels, so mods built for an
  older minor keep compiling; a mod that needs the new minor calls the new function and fails on older Cores.
  `RELEASE_REQUIRES` = the version new release mods require (raise it only when they use something new).
  Patch = same functions, minor = new functions / assets, major = a removal (avoid).
- The Studio compares the installed Core (`content\scripts\local\hairworkforall_core.ws` + `version.json`) with the
  repo's `CORE_VERSION` (status bar + Export panel) and warns when the Core is missing, incomplete or older (the early texture Core has no script and is reported incomplete).

## Load order

The folder name `mod0000HairworkForAllCore` sorts before every `mod000_*`, `modA*`... folder. For scripts the order
does not matter (global functions are visible to every mod); the script path `content\scripts\local\hairworkforall_core.ws`
is ours only (`install` checks that no other mod ships it).

## Protection

The Core is now a script plus a version file: the dependency is technical (the game will not start a release mod without
it) but trivial to remove. A user can delete a mod's `hwfa_req_*.ws` locally; that is personal use and fine. What keeps
the requirement in shared mods is the licence (`core/LICENSE.md`; the Hairwork Studio License section 2 for Studio mods:
no re-upload of the Core, no copying its functions into another mod, no publishing a mod with the check removed) and the
Nexus permissions of the mod pages.
