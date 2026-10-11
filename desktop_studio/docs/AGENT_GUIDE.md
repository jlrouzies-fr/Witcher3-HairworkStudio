# Hairwork Studio: guide for AI agents

How to drive the desktop Studio (`desktop_studio/`) without a human: headless agent scripts, UI test scripts, packs, exports. Everything here is checked against the code (`src/main.cpp`, `src/agent.cpp`, `src/uitest.cpp`, `tools/desk_prep.py`, `tools/desk_export.py`). The human manual is `docs/guide/guide.html`. Read `AGENTS.md` (hard rules) first, then `desktop_studio/README.md` for internals.

## Hard rules for Studio work

1. **No real input, ever.** Never use SendInput, pywinauto, AutoHotkey, screen clicking, `SetForegroundWindow` or any OS-level mouse / keyboard / focus. Drive the UI only with `--ui-test` (events injected into Dear ImGui) or the headless `--agent` runner. A normal launch takes the user's screen; a ui-test run opens the window **without activation, off the visible desktop** (`main.cpp`).
2. **No game runs** unless the task says so. The Studio needs no game process. Never kill a `witcher3.exe` you did not start.
3. **Approved builds are frozen** (`tools/hairworks/APPROVED.json`): rebuilding one must give byte-identical output. New generator behaviour is an **opt-in config key** and never a default. A shared-code fix gets a legacy switch set in approved configs. The Studio itself never touches approved configs: its export writes a *proposal* (`<char>_<tag>_config.json`); a new variant needs a new name `<char>_l<round>_<tag>`.
4. **NVIDIA SDK material never enters git** (`sources/` is git-ignored; `py tools/sdk_guard.py --tracked` and the `.githooks/pre-commit` hook enforce it; run `git config core.hooksPath .githooks` once per clone). Do not paste SDK headers or docs anywhere. Only the runtime DLL ships in release packaging.
5. Do not edit `desktop_studio/packaging/` or release settings unless that is your task; commit messages carry no model identifier except the trailer the session gives you.
6. Never overwrite another agent's work: re-read a file before editing; keep `build/` and `results/` out of git.

## Build and launch

| What | Command (repo root) |
|---|---|
| Build if missing or stale, then run | `Launch_Groom_Studio_Desktop.bat` |
| Force a rebuild | `Launch_Groom_Studio_Desktop.bat --rebuild` |
| Manual build | `cmake -S desktop_studio -B desktop_studio/build -G "Visual Studio 17 2022" -A x64 [-DHAIRWORKS_SDK_DIR=<sdk>]` then `cmake --build desktop_studio/build --config Release` |
| The exe | `desktop_studio\build\Release\HairworkStudio.exe` (shaders `*.cso` and `assets\` sit next to it) |

The .bat rebuilds when the exe is missing or any file in `src/`, `shaders/`, `CMakeLists.txt` is newer than it, passes its first 9 arguments to the exe, waits for the window to close and prints the last 15 log lines on a crash (`CRASH` line = function + source line). It `pause`s on a crash or build failure: when running from an agent call the exe directly so nothing blocks. Do not run two builds in the same `desktop_studio/build` at once.

Needs: VS 2022 Build Tools (C++ workload, Windows SDK), CMake >= 3.20, the HairWorks 1.2.1 SDK in `sources/HairWorks-r1_2_1-15-distro/HairWorks` (or `HAIRWORKS_SDK_DIR`), Python (`py`) with `requirements.txt` installed.

**Python.** A dev build calls the pipeline through the `py` launcher (Settings > Python). A release folder carries its own private CPython 3.13 (`python\python.exe` next to the exe, with numpy / Pillow / scipy / lz4 in `python\Lib\site-packages`; made by `desktop_studio/packaging/embedded_python.py` during `package_release.py`, downloads cached in `build/python_embed/`, SHA-256 pinned): the exe uses it by default when it exists, so players need no Python. A saved Settings value other than `py` / empty overrides it. The bundled `python313._pth` puts the release root, `tools`, `tools\hairworks`, `desktop_studio	ools` and `core` on `sys.path`. To run a pipeline script by hand in a release folder use `python\python.exe <script>`. Do not commit the runtime (`build/` is git-ignored; the public export leaves it out).

## Command line

Parsed in `main.cpp parseArgs`:

| Flag | Meaning |
|---|---|
| `--agent <script.json>` | Headless runner (no window shown, console output, log `build\desktop_studio\agent.log`) |
| `--ui-test <script.json>` | Scripted UI run (log `build\desktop_studio\desktop_studio.log`) |
| `--cfg <cfg>` | Open this build at start |
| `--project <file>` | Open a `*.groomproj.json` at start |
| `--sdk <dir>` / `--dll <path>` | HairWorks SDK folder / DLL (override Settings) |
| `--screenshot <png> --frames N [--exit]` | Save the back buffer at frame N (default 240); `--exit` closes after |
| `--view front\|back\|left\|right\|34\|back34`, `--motion <prefix>`, `--guides`, `--close-lod` | Initial camera / motion / overlays |
| `--no-splash` | Skip the splash |

Environment: `GROOM_STUDIO_NOACTIVATE=1` (window opens without activation off-screen; always on for `--ui-test`), `HAIRWORKS_SDK_DIR`, `HW_DIRECT_ASSET=1` (skip the save/load asset round trip), `HW_BONE_STIFF_EMU` (bone-stiffness emulation strength). Settings file: `%LOCALAPPDATA%\HairworkForAll\desktop_studio.json`.

Exit codes of `--agent`: 0 ok, 2 script unreadable, 3 HairWorks runtime not loaded, 4 cannot open the build or project, 5 unknown material preset, 6 export blocked or failed, 7 unknown step.

## Making a pack (`desk_prep.py`)

The Studio opens a **pack** (`build\desktop_studio\<cfg>\desk.json` + `tex_*.png`). The Studio runs the prep itself when needed (`openConfig(cfg, reprep)`); by hand:

```
py desktop_studio\tools\desk_prep.py <cfg>            # build/refresh the pack (builds the fur when the config hash changed)
py desktop_studio\tools\desk_prep.py <cfg> --build    # force a pipeline rebuild of the fur first (nothing installed)
py desktop_studio\tools\desk_prep.py <cfg> --check    # prints 'current' (exit 0) or the reason it is stale (exit 1)
py desktop_studio\tools\desk_prep.py --list           # configs with a built fur; '*' = approved
py desktop_studio\tools\desk_prep.py --catalog        # character catalog JSON for the picker
```

Rules:
- The pack always reads the config's **current** pipeline build (`build\desktop_studio\furs\<cfg>__<comp>.redfur` + a `<cfg>.src.json` stamp: sha1 of `chars\<cfg>.json` and its edits file), never a lab round copy.
- A pack is stale when `prep_version` (`PREP_VERSION` in `desk_prep.py`, `kPrepVersion` in `src/app.cpp`) is older, a source file listed in `desk.json src_files` is newer, or its fur is gone. Bump both constants when `desk.json` changes shape.
- Prepping and exporting install nothing into `<game>\mods`.

## Agent scripts (`--agent`)

```
desktop_studio\build\Release\HairworkStudio.exe --agent script.json
```

The runner loads the build (`cfg`, optional `"reprep": true`) or a project (`"project": "<path>"`), settles the simulation at rest, then runs `steps` in order. Top-level keys: `cfg`, `reprep`, `project`, `out` (default `build\desktop_studio\agent`), `steps`.

```json
{"cfg": "guillaume_l124_locks_hug", "out": "C:/tmp/run1",
 "steps": [
  {"do": "ops", "file": "tools/hairworks/edits/guillaume_user1.json"},
  {"do": "select", "where": {"back": true, "z_min": -0.03}},
  {"do": "delete"}, {"do": "shorten", "where": {"back": true}, "keep": 0.8}, {"do": "duplicate", "step_mm": 6},
  {"do": "curl", "where": {"back": true, "tip_below": 0.15},
   "params": {"mode": "roll", "start": 0.6, "radius": 0.015, "turns": 1.2, "shrink": 0.6, "band": false}},
  {"do": "material", "name": "Fine strands (Yen", "keep_colours": true},
  {"do": "look", "set": {"physicalMaterials.stiffness.tipStiffness": 0.4}},
  {"do": "hold_curls"},
  {"do": "stiffness", "where": {"front": true}, "s": 0.9, "r": 1.0},
  {"do": "rig", "tod": 0, "sun": 2.0, "shadows": true, "quality": 2},
  {"do": "motion", "preset": "walk", "seconds": 3, "speed": 1, "amount": 1, "reset": true},
  {"do": "settle", "seconds": 2},
  {"do": "shots", "tag": "v1", "shots": [0, 2, 3, 4, 5, 15], "pair": true, "mask": false, "size": 1400},
  {"do": "bench", "w": 2560, "h": 1440, "frames": 240, "quality": 2, "view": "34", "preset": "idle"},
  {"do": "save_project", "path": "C:/tmp/run1/x.groomproj.json"},
  {"do": "export", "tag": "agent1", "mod": "modGuillaumeHairworks", "release": false}]}
```

Step reference (all from `agent.cpp`):

| `do` | Keys | Notes |
|---|---|---|
| `ops` | `file` (an edits JSON with `ops`) or `ops` (inline list) | appends ops, replays, reports `unmatched` roots (4 mm match) |
| `select` | `where` | sets the selection |
| `curl` | `params`, optional `where` | `params` = `CurlParams` JSON (`mode` ringlet / roll / wave, `start`, `radius`, `turns`, `ramp`, `hand`, `jitter`, `shrink`, `side`, `band`, `zigzag`); reports `cvs_per_turn` and a warning under 6 |
| `delete` / `shorten` / `duplicate` | `where`, `keep` (0..1, default 0.8), `step_mm` (default 6) | without `where` uses the current selection |
| `material` | `name` (prefix match of a preset name), `keep_colours` (default true) | exit code 5 and `error` when no preset matches |
| `look` | `set` {param path: number or JSON} | param paths as in `src/params.cpp` (e.g. `graphicalMaterials.color.rootColor`); caps are re-enforced |
| `hold_curls` | | raises stiffness / flattens curves for held curls |
| `stiffness` | `where` (default all), `s`, `r` | per-guide stiffness map values |
| `rig` | `tod` (0..8), `sun`, `sky`, `exposure`, `colour_gain`, `spec_gain`, `spec_power`, `tex_gain`, `self_shadow`, `floor`, `sun_az`, `sun_el`, `shadows`, `horizon`, `ground`, `quality` (0 fast, 1 balanced, 2 high), `ao`, `tone`, `sun_size`, `shadow_soft`, `sun_col` / `sky_col` / `horizon_col` / `ground_col` ([r,g,b]), `debug`, ... | `tod` is applied first, explicit keys override it |
| `motion` | `preset` (prefix: idle, walk, run, turn, nod, spin, wind, rest), `seconds` (2), `speed`, `amount`, `reset` | runs the simulation for `seconds` at 60 Hz |
| `settle` | `seconds` | rest pose, then simulate |
| `simulate` | `on` | false = hair on the skinned rest shape |
| `bone_stiffness` | `amount` 0..1 | W3 stiffnessBone emulation (preview) |
| `shots` | `tag`, `shots` (default `[0,2,3,4,5,15]`), `size` (1400), `pair` (true), `mask` (false) | HairLab cameras from the pack; writes `<tag>_shot<n>.png`, `<tag>_<view>_PAIR.png` (vanilla left, ours right) when a vanilla still exists, `<tag>_shot<n>_mask.png` (flat-red hair-only pass) |
| `bench` | `w`, `h`, `frames`, `quality`, `view`, `preset` | GPU-synced frame time of the viewport scene: `ms_median`, `ms_p95`, `fps_median`; writes `bench_q<quality>.png` |
| `dump_params` | `path` | every fur value next to the SDK instance value |
| `save_project` | `path` | `*.groomproj.json` |
| `export` | `tag`, `mod`, `release` | see Export below; `release: true` adds the Core check info (`core`: installed, version, required) |
| `report` | | no-op (the budget is attached to every step anyway) |

`where` selectors (root-based, edit frame): `all`, `back` / `front` (vs head centre y), `left` (+x) / `right`, `z_min` / `z_max` (m, relative to head top), `tip_below` (m), `roots` (list), `indices`.

Outputs in `out`: `report.json` (per step result plus `budget`: `hairs`, `hairs_close`, `ref`, `ref_hairs_close`, `ratio`, `over_limit`; final block with `config`, `guides`, `faces`, `ops`, `unmatched`, `hairs_live`, budget, `material`, `look_changes`, `stiffness_map`) and `edits.json` (the resulting edit ops). Unknown step: `error` and exit code 7.

Shots: the character's own raw cameras are used when a round exists (`results/*/raw`), otherwise the generic Guillaume rig; 1400 px by default, camera and `fovy` taken from each shot's JSON in the pack.

Calibration helpers: `py desktop_studio\tools\calib.py <cfg> <out> <tag> --sheet sheet.jpg` (needs `mask: true`), `lookcompare.py`.

## UI test scripts (`--ui-test`)

```
desktop_studio\build\Release\HairworkStudio.exe --ui-test script.json
```

Events go straight into ImGui (`AddMousePosEvent`, `AddMouseButtonEvent`, `AddKeyEvent`): the OS cursor, keyboard and focus are never touched. Widgets are found **by label** using ImGui's test-engine item hooks. The splash is off unless the script sets `"splash": true`. Time is fixed at 1/60 s per frame. After the last step the app closes itself and writes `<out>\ui_test_result.json` (`steps` with `ok` / `note`, `failures`, `crash`). Check it: a failed step does not abort the run.

Top level: `{"out": "<dir>" (default build\desktop_studio\uitest), "splash": false, "steps": [...]}`. Screenshots are the back buffer at the window's size (about 3334 x 1969 on a 4K screen).

Steps (`uitest.cpp`):

| `do` | Keys |
|---|---|
| `click` / `dclick` / `rclick` | `label` (+ `nth`, 0-based among identical labels) or `xy` [x, y] in pixels; optional `rows` (n widget rows below the label), `dx` (x offset). Waits up to 60 frames for the widget to appear, then fails with `widget not found` |
| `hover` | `label` / `xy`, `dx` (tooltips, hover states) |
| `move` | `xy` |
| `drag` | `xy` -> `to`, `button` (0 left, 1 right) |
| `wheel` | `xy`, `dy` |
| `key` | `key` (A-Z, 0-9, F1, F6, F12, Space, Escape, Enter, Delete, Tab, Period), `ctrl`, `shift` |
| `text` | `text` (typed characters) |
| `wait` | `frames` (default 30) |
| `wait_job` | `max_frames` (3600): waits for a background load / build |
| `shot` | `name` (PNG saved to `out`) |
| `expect` | any of: `loaded`, `config`, `picker`, `simulate`, `ops`, `motion` (prefix), `material` (prefix), `widget` (label visible), `tod`, `shadows`, `paused`, `tool` (0 select, 1 move, 2 comb, 3 curl, 4 paint), `editpose`, `hairdebug`, `light`, `camshot`, `symmetry`, `guides`, `exported`, `collapsed`, `style`, `loaded_style`, `editor_style` |
| `list_widgets` | writes `<out>\widgets.json`, every label seen in the last frame (use it to find labels) |

Any step may add `then_wait` (frames to wait afterwards, default 2).

Label conventions: section headers by name (`Material`, `Look`, `Physics and motion`, `Stiffness map`, `Curl tools`, `Symmetry`, `Edit settings`, `Export`); slider rows by parameter label (`stiffness`, `root stiffness`); rail `##sel ##mov ##comb ##curl ##paint ##sym ##undo ##redo ##dup ##short ##del ##shortcuts ##settings`; top bar `##tb_guides ##tb_hair ##tb_coll ##tb_bones ##tb_wire ##tb_plane Front##tb_cam4 ¾##tb_cam3 Side##tb_cam2 Back##tb_cam0 ##tb_frame ##tb_tod ##tb_shadow ##tb_collapse`; bottom bar `##tb_play ##tb_stop ##tb_step ##tb_motion ##tb_wind`; title bar `File Edit View Help ##crumb ##crumb_style Characters##chars Export mod##title`; picker `card` (use `nth`), `##q` (search box), `Open##drawer`, `##drawer_close`, `All##st`..`Not started##st`, `Painted##pt`, `<type>##ht`, `<part>##gp`, `<style>##style`; popups `Close##settings`, `Close##shortcuts`, `Close##about`, `Advanced...##light`.

Gotchas learned from running it:
- Clicking outside an open popup only closes it; click again.
- The inspector clips sections below the fold, and widgets that are not drawn cannot be found: collapse open sections (Material, Export) first, or scroll with `wheel`.
- Mouse position matters for tooltips: `move` to a blank area (for example `[1400, 700]` in the picker) before a `shot`.
- Opening the Settings modal by `click` on `##settings` did not work while another floating window (Light) was open; close it first. With it closed, `move` onto the gear and a `click` by `xy` (about `[53, 1840]` at 3334 px wide) opened it.
- Picking a card then `Open##drawer` is more reliable than a double click when a hairstyle must be chosen first.
- Do not leave the Light window open for later shots (its close button has no label; click its X by `xy`, or toggle View > Light settings).

Existing scripts: `tests/ui/picker_editor.json` (picker filters, drawer, hairstyles, editor style switcher), `tests/ui/editor_full.json` (every inspector section, both bars, view / tool / camera / light keys, menus, settings, shortcuts, export), `tests/ui/bone_stiffness.json` (needs `--cfg guillaume_l124_locks_hug`). The manual's screenshots were made with a script of this kind (`docs/guide/img/`).

Run recipe from an agent: write the script under a scratch folder, set `"out"` there, run the exe in the foreground with a timeout (the process exits by itself), then read `ui_test_result.json` and look at every PNG before using it.

## Export

UI: Export section (`Export edits + mod`, `Export for release (requires Core)`); agent: `{"do": "export", "tag": ..., "mod": ..., "release": ...}`. Both call `exportAll`, which writes `<tag>_edits.json`, `_look.json`, `_geo.json` and runs `desk_export.py`:

```
py desktop_studio\tools\desk_export.py --cfg <cfg> --edits <x_edits.json> --look <x_look.json> --geo <x_geo.json> --mod <modName> --tag <tag> [--limit 1.2] [--stiffness <map.png>] [--density-png <p>] [--colour-png <p>] [--release]
```

- `--mod` must start with `mod` and use letters, digits and `_` only. The export is **blocked** when the budget ratio in `_geo.json` exceeds `--limit` (default 1.2) (agent exit code 6).
- Output: `build\desktop_studio\<cfg>\export\`: `<char>_<tag>_edits.json`, `_look.json`, `_geo.json`, `_config.json` (a pipeline config proposal with `edits` and `look.extra_set`; rename it to a new `<char>_l<round>_<tag>` and copy the edits to `tools\hairworks\edits\` to build it with HairLab; with `--release` it carries `"core": true`), `<mod>\content\blob0.bundle` + `metadata.store`, `INSTALL_INSTRUCTIONS.txt`, and `<mod>_<tag>.zip`. Non-default hairstyles use `<char>_<style>_<tag>_...` names.
- It starts from the character's OWN built redfur: geometry is rewritten only when there are ops, look values only when they differ from the build, stiffness map only when painted (opt-in, preview not verified in game). Physics, LOD, density mask and collision stay as built.
- **Release export** (`--release`): the same self-contained mod plus `content\scripts\local\hwfa_req_<mod>_<crc>.ws` (calls `HairworkForAllCore_Required_GetItFromNexus13996()` and `HairworkForAllCore_v1_0()`), `REQUIREMENTS.txt`, `hairworkforall_mod.json` (`requires_core`, and the Graphics Control requirement with "Hair root transparency mode" = 0), and requirement lines in `INSTALL_INSTRUCTIONS.txt`. Without the Core installed the game's script compiler fails with `Could not find function 'HairworkForAllCore_Required_GetItFromNexus13996'`. The Core state is read from `<game>\mods\mod0000HairworkForAllCore\content\scripts\local\hairworkforall_core.ws` plus `version.json`, against `CORE_VERSION` in `core/hwcore.py`; see `core/README.md`.
- Nothing is installed. To test in game, copy the mod folder into `<game>\mods` through the repo's build scripts (HairLab `install`), never by hand-swapping `metadata.store` or `blob0.bundle` (a hand-swapped `metadata.store` once broke all game text).

## Approved-build rules for Studio changes

- `desk_prep` rebuilds of every approved config must stay **byte-identical** to `mods\mod<Char>Hairworks` (checked 2026-10-11 for Guillaume, c04ma, Regis, Dandelion, Triss, Keira; Vesemir `vesemir_r31a`). Run the check after touching `hairpipe.py`, `redfur.py`, `strand_edits.py` or `desk_prep.py`.
- New op types or keys are opt-in and appear in an edits / config file only when used (example: `reshape` for baked curls, the painted stiffness texture).
- Keep `tools/hairworks/strand_edits.py check <edits> build\hairbatch\edit\<cfg>.json` passing for edits the Studio exports.
- Update `desktop_studio/README.md`, and `tools/hairworks/HAIRWORK.md` in the same commit as a behaviour change.

## Pointers

- Operating model and hard rules: `AGENTS.md`; skill `witcher-3-hairwork` (`.claude/skills/witcher-3-hairwork/SKILL.md`, `references/rules.md`, `workflow.md` section 6 "Groom Studio", `techniques.md` for opt-in config keys).
- Full HairLab log: `tools/hairworks/HAIRWORK.md` (sections 0, 2, 7 and the last rounds); internals: `tools/hairworks/lab/SPEC.md`, `tools/hairworks/PIPELINE.md`.
- Studio internals and numbers: `desktop_studio/README.md` (How it works, Agent CLI, UI test mode, Rendering, Start screen); look and widgets: `desktop_studio/design/ui_direction1/IMPLEMENTATION.md`; licence: `desktop_studio/LICENSE.md`.
- Source map: `app.cpp` (settings, pack loading, export, Core check), `ui.cpp` (title bar, inspector, popups, input), `toolbar.cpp` (viewport bars), `picker.cpp` (start screen), `stylesw.cpp` (hairstyle switcher), `agent.cpp`, `uitest.cpp`, `groom.cpp` (edit ops), `curls.cpp`, `lighting.cpp`, `motion.cpp`, `hairsdk.cpp` (HairWorks runtime).
