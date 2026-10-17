---
name: hairwork-studio
description: Drive Hairwork Studio (native HairWorks hair editor for The Witcher 3 next-gen 5.0) through its MCP server - open a character, render and compare against vanilla, edit strands, save under a new name, export a mod or a shareable hair pack. Use for any Witcher 3 HairWorks / hair strands / Hairwork Studio / hair pack request.
---

# Hairwork Studio for agents

Hairwork Studio converts a Witcher 3 character's hair cards to real NVIDIA HairWorks strands built from the player's OWN game install, lets you edit and simulate them, and exports a mod. The MCP server (`hairwork_studio_mcp.py`) runs the Studio headless (`HairworkStudio.exe --serve`: no window, no input, no focus) and exposes it as tools. Every render tool returns the image, so you edit and SEE the result in one loop.

## 0. Setup checklist (do this before anything else)

1. **The Studio.** Either the release zip (`gh release download --repo jlrouzies-fr/Witcher3-HairworkStudio --pattern "*.zip"`, unzip it somewhere user-writable such as `C:\HairworkStudio`, NOT Program Files) or a clone of `https://github.com/jlrouzies-fr/Witcher3-HairworkStudio` built with CMake (needs the NVIDIA HairWorks 1.2.1 SDK, which the user must download from NVIDIA; never ask for it to be committed). Prefer the release zip: it bundles the exe, the NVIDIA runtime DLL and a private Python.
2. **The game.** The user needs The Witcher 3 with the next-gen patch 5.0 (DX12) installed. Ask for the game folder (the one containing `content\content0` and `mods`). The Studio auto-detects Steam / GOG / Epic folders; otherwise set the environment variable `HWFA_GAME=<game folder>` for the MCP process (or Settings > Game folder in the Studio window).
3. **The MCP.** Register it with the bundled Python (release zip, `<studio>` = the unzipped folder):
   `claude mcp add hairwork-studio -- <studio>\python\python.exe <studio>\desktop_studio\mcp\hairwork_studio_mcp.py`
   Other MCP clients (stdio): `{"mcpServers": {"hairwork-studio": {"command": "<studio>\\python\\python.exe", "args": ["<studio>\\desktop_studio\\mcp\\hairwork_studio_mcp.py"], "env": {"HWFA_GAME": "<game folder>"}}}}`
   From a clone use `py` (Python 3.10+ with `pip install -r requirements.txt`) instead of the bundled python. `HAIRWORK_STUDIO_EXE` overrides the exe path, `HWS_OUT` the output folder (default `<studio>\build\desktop_studio\mcp`). Restart the agent session after registering so the tools appear.
   Without MCP registration you can use the shell wrapper: `<python> <studio>\desktop_studio\mcp\hws.py daemon` (background), then `hws.py <tool> '<json>'`, `hws.py list`, `hws.py stop`; images are saved as PNG files, open them with your image viewer.
4. **Community packs (optional).** Shared hair packs live in `https://github.com/jlrouzies-fr/Witcher3-HairworkCommunity`. Clone it to browse or to submit one (the Studio's Community window does the same from the UI; it needs a GitHub account and the `gh` CLI).

## 1. Hard rules

- **Never ship game files.** Everything the Studio builds from the game (packs, meshes, textures, `.redfur`, bundles) stays on the user's PC. Share only edits / pack JSON and exported mods the Studio makes. Never upload CD Projekt files, third-party mods or their files.
- **No NVIDIA SDK material in git, ever** (headers, sources, docs, samples, Viewer). Only the runtime DLL ships inside the Studio release, as the NVIDIA GameWorks EULA allows. Never paste SDK code or docs into a repo, issue or pack.
- **Save under a new name** every time (`studio_save`): `<char>_<tag>`; never overwrite a build you did not make. Approved builds shipped with the Studio are never written by the tools.
- **Never run or kill the game** unless the user asks. Never kill a `witcher3.exe` or a `HairworkStudio.exe` you did not start. The MCP's own Studio process exits when the session ends.
- **No real input.** Never click, type or focus windows to drive the Studio; the MCP is the interface. If the user has a Studio window open, leave it alone.
- Mods made with "Export for release" need two other mods installed by everybody who plays them: Hairwork for All - Core (Nexus 13996) and Graphics Control (Nexus 13349) with PT hair > "Hair root transparency mode" = Mode 0. Tell the user.
- Do not include the user's e-mail, API keys or tokens anywhere.

## 2. Budget and path tracing (read before adding hair)

- **Hair budget.** `studio_status` reports a budget ratio vs the reference build. It is INFORMATION ONLY (user 2026-10-15: "REMOVE that budget limit"): never a reason to stop, refuse or thin guides; no tool refuses or drops guides for it and export never blocks on it. GPU cost is judged from in-game measurements. HairWorks grows 64 x density hairs per GROWTH FACE whatever its size, so cost comes from faces, not guide count. `studio_free_budget` is an explicit tool only; do not run it unless asked.
- **Path tracing.** Players with the game's path tracing on pay GPU time per hair (growth faces, low clump, strong curl). More strands = more ms. Do not densify "just in case"; fill only visible bald spots and check the budget after each pass.
- Scalp coverage in the green overlay ignores bald skin BELOW the painted hairline: always look at `studio_compare` too.
- Hair in front of the eyes: keep `guides_over_face` 0 and `studio_eye_check` `worst_increase` < 0.05.

## 3. The loop

1. `studio_list_configs` -> pick a build (it lists only builds already made on this PC, so it is empty on a fresh install: then open a bundled base config by name, e.g. `lambert`; the names are the files in `<studio>\tools\hairworks\chars\`). `studio_open {"cfg": "<name>"}` (or `{"project": "<file>.groomproj.json"}`). The FIRST open of a character builds its strand pack from the player's game files and can take several minutes: wait, do not retry.
2. `studio_status` (guide / face counts, budget, unmatched roots). `studio_shots` lists the cameras.
3. `studio_compare` (vanilla cards | as opened | current, `shots: [15, 4, 3]`, `scalp: true`) and `studio_render` (`overlay` none | scalp | mask | guides, `shot` or `eye`/`target`/`fovy`, `crop`) to see.
4. Edit with the tools below (each takes `dry_run: true` to measure and restore; `show: true` returns a render). Re-measure with `studio_scalp_coverage`, `studio_hairline_check`, `studio_eye_check`, `studio_color_check`. `studio_undo` undoes one whole tool call.
5. `studio_look_set` / `studio_materials` for colour and physics, `studio_simulate` for gravity / wind / motion, `studio_rig` for lighting.
6. `studio_save` under a NEW name. Then export (section 5).

Start plain: look first, then fix one defect at a time, only defects you can SEE in a render, and compare against vanilla at native size. After two edits with no visible change, stop and diagnose (`studio_guides`, `studio_faces`, `studio_pick`) instead of tuning more.

## 4. Tools

Session: `studio_list_configs`, `studio_open`, `studio_status`, `studio_fur` (Beard mode: edit the beard fur instead of the hair, on builds with `has_beard`), `studio_shots`, `studio_save`, `studio_batch` (several tools in one call), `studio_undo`, `studio_redo`, `studio_reset_edits`.
Seeing: `studio_render`, `studio_closeup`, `studio_compare`, `studio_scalp_coverage`, `studio_pick` (what is under a pixel), `studio_pick_region`, `studio_guides`, `studio_guide`, `studio_faces`, `studio_select`, `studio_guide_view` (front layer only / hide behind the body; `select: true` selects the visible guides).
Basic edits: `studio_delete`, `studio_duplicate`, `studio_densify`, `studio_shorten`, `studio_lengthen`, `studio_move`, `studio_move_root_to_scalp`, `studio_set_point`, `studio_reshape`, `studio_curl`, `studio_mirror`, `studio_ops` (raw edit ops).
Hairline, parting and cleanup: `studio_hairline_check`, `studio_align_to_hairline`, `studio_flow_comb`, `studio_fill_scalp`, `studio_plant_parting`, `studio_hairline_fill`, `studio_feather_hairline`, `studio_fill_below_hairline`, `studio_centre_parting`, `studio_plant_fringe`, `studio_parting_roots`, `studio_lay_down`, `studio_smooth_flow`, `studio_push_out`, `studio_trim_growth`, `studio_dedupe`, `studio_unzigzag`, `studio_hairnet`, `studio_layer_lift`, `studio_inflate`, `studio_free_budget`.
Face and ears: `studio_clear_ear`, `studio_route_behind_ears`, `studio_clear_eye`, `studio_eye_check`, `studio_outside_vanilla`.
Drastic reshape toward the game's own hair cards (opt-in, Studio-only): `studio_conform_to_cards`, `studio_hug_cards`, `studio_rebuild_from_cards` (start here when the starting groom is far from vanilla), `studio_regrow`, `studio_keep_cards`, `studio_card_offset` (move a kept card piece, saved + exported), `studio_make_bun`, `studio_twist_roll`.
Look and physics: `studio_look_get`, `studio_look_set`, `studio_materials`, `studio_stiffness`, `studio_rig`, `studio_simulate`, `studio_color_check`.
Read each tool's description and schema from `tools/list`: it is the source of truth for arguments. Edit coordinates: metres, +X = character's left, +Y = forward, +Z = up. Selectors (`where`) accept guide indices, regions, `selection`.

Recipe for bald spots and a parting: top-camera `studio_render overlay: "scalp"`, `studio_pick` two or three points on the parting, `studio_plant_parting` with that line, `studio_lay_down` for frizz (do NOT delete short guides first: they usually hold scalp coverage), `studio_fill_scalp` 2-3 passes re-measuring `studio_scalp_coverage` until it stops dropping, then `studio_compare`.

## 5. Export and sharing

- **Mod for yourself** (works without the Core, for testing): the Studio's plain Export. **Mod to share**: "Export for release" (needs Core + Graphics Control for everybody). Both write to `<studio>\build\desktop_studio\<cfg>\export\` (an `INSTALL_INSTRUCTIONS.txt` and a zip). Nothing is ever installed into the game's `mods` folder by the Studio: the user copies the folder. Never hand-swap `metadata.store` or `blob0.bundle` inside an installed mod.
- `studio_save` writes the project / edits under `<studio>\build\desktop_studio\mcp\<name>\`; reopen it later with `studio_open {"project": ...}`.
- **Applying packs headless:** the `--agent` runner (see AGENT_GUIDE.md) has the steps `apply_pack` (`{"do":"apply_pack","dir":"<pack folder>"}`, same as Community > Apply, including the new baked packs that need Studio 0.2.1) and `dump_geo` (writes the current groom as JSON).
- **Hair pack** (data only, no game files) for the community: export from the Studio's Community window (My packs) and publish through the pull request flow, or follow the Witcher3-HairworkCommunity README. CI checks packs.
- Credit "made with Hairwork Studio" and the Nexus link https://www.nexusmods.com/witcher3/mods/13996 in anything you share.

## 6. Troubleshooting

- "HairworkStudio.exe not found": set `HAIRWORK_STUDIO_EXE`. Game not found: set `HWFA_GAME`.
- The Studio log is `<studio>\build\desktop_studio\serve.log`. If the Studio process crashes the MCP restarts it and replays your edits (the reply says so).
- Two agents at once: give each its own `HWS_SESSION` (folder name) and `HWS_PORT` in the environment.
- Deeper reference (in the release zip: `docs\AGENT_GUIDE.md` and `docs\guide\guide.html`): `desktop_studio/docs/AGENT_GUIDE.md` (command line, `--agent` scripts, export internals) and the human manual `desktop_studio/docs/guide/guide.html`.
