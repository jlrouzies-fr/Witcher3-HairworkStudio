# Hairwork Studio

Hairwork Studio is a native Windows editor and simulator for **NVIDIA HairWorks** hair in **The Witcher 3** (next-gen patch 5.0, DX12).
It converts a character's vanilla hair cards to real HairWorks strands built from *your own* game install, lets you edit and
simulate them with the real HairWorks 1.2.1 runtime (gravity, stiffness, damping, wind, collisions, density map, LOD) and exports a
Witcher 3 mod. Part of **Hairwork for All**.

- Nexus Mods (downloads, the Core mod): https://www.nexusmods.com/witcher3/mods/13996
- User guide: [`desktop_studio/docs/guide/guide.html`](desktop_studio/docs/guide/guide.html), agent / scripting guide: [`desktop_studio/docs/AGENT_GUIDE.md`](desktop_studio/docs/AGENT_GUIDE.md)

## Licences (which covers what)

| Part | Licence |
|---|---|
| `desktop_studio/` (the Studio: source, shaders, tools, assets made for it) | **Hairwork Studio License**, source-available, not open source: [`desktop_studio/LICENSE.md`](desktop_studio/LICENSE.md). Personal use and modification are free; shared hair mods must keep the Core requirement and credit; no commercial use. |
| `core/` (Hairwork for All - Core, the dependency mod) | **Core license**: [`core/LICENSE.md`](core/LICENSE.md) |
| everything else (`tools/`, the pipeline) | MIT, [`LICENSE.md`](LICENSE.md) |
| Third-party libraries in `desktop_studio/third_party/` and fonts / icons in `desktop_studio/assets/fonts/` | their own licences (Dear ImGui MIT, nlohmann/json MIT, stb public domain / MIT, Cinzel + Inter SIL OFL 1.1, Lucide ISC) |
| **NVIDIA HairWorks** | proprietary, NVIDIA GameWorks EULA. **Not in this repository.** |

## What is NOT here

- **The NVIDIA HairWorks SDK** (headers, sources, docs, samples, Viewer, DLLs). Its source, headers and documentation are NVIDIA
  Confidential Information under the GameWorks EULA, so they must never be committed. A pre-commit hook and a CI job
  (`tools/sdk_guard.py`) block them. Only the Nexus *release* ships the runtime DLL `NvHairWorksDx11.win64.dll`, as the EULA allows.
- Any file from the game, third-party mods, captures or results. The pipeline reads the game's files from your install at run time.

## Build the Studio

Needs Windows 10/11 x64, Visual Studio 2022 Build Tools (Desktop development with C++, Windows SDK 10.0.26100 or newer) and CMake 3.20+.
You also need the **NVIDIA HairWorks 1.2.1 SDK** (`HairWorks-r1_2_1-15-distro`) from NVIDIA's GameWorks download page: accept their EULA
and unzip it anywhere; it is only read at build time (headers, shader header) and run time (runtime DLL).

```
cmake -S desktop_studio -B desktop_studio/build -G "Visual Studio 17 2022" -A x64 -DHAIRWORKS_SDK_DIR=<...>\HairWorks
cmake --build desktop_studio/build --config Release
```

`HAIRWORKS_SDK_DIR` must contain `include/Nv/HairWorks/NvHairSdk.h` and `bin/win64/NvHairWorksDx11.win64.dll`. The exe, compiled
shaders and assets land in `desktop_studio/build/Release/`. Details: [`desktop_studio/README.md`](desktop_studio/README.md).

## Run

Building from source needs Python 3.10+ (tested 3.13) with the pipeline requirements: `py -m pip install -r requirements.txt`.
The Nexus release does not: `desktop_studio/packaging/package_release.py` bundles a private embedded Python (the python.org "embeddable package" +
the pinned pip packages, see `desktop_studio/packaging/embedded_python.py`; downloaded and cached under `build/python_embed/` at package time,
not stored in this repository) as `python/` next to the exe, so players install no Python.
Start `desktop_studio/build/Release/HairworkStudio.exe`. It finds the pipeline (`tools/`) by walking up from the exe. Set your
Witcher 3 folder in Settings > "Game folder" (or the environment variable `HWFA_GAME`) if it is not found automatically.
The first time you open a character the Studio builds its strand pack from your game files.

## Release export and mod requirements

"Export for release" makes a mod that needs, for everybody who plays it:

- **Hairwork for All - Core** (Nexus 13996): https://www.nexusmods.com/witcher3/mods/13996 (source in `core/`)
- **Graphics Control** with PT hair > "Hair root transparency mode" = Mode 0: https://www.nexusmods.com/witcher3/mods/13349

## Agents / MCP

The Studio ships an **MCP server** (`desktop_studio/mcp/hairwork_studio_mcp.py`, stdio, no third-party packages, runs on the Python bundled in the release). It runs a hidden, live Studio session (`HairworkStudio.exe --serve`: no window, no input, no focus) and exposes about 70 `studio_*` tools: open a character, render and compare against the vanilla hair cards, pick / densify / duplicate / reshape / comb strands, fill bald spots, check hairline and eyes, change look and physics, save under a new name. Render tools return the picture itself, so a coding agent (Claude Code or any MCP client) edits and sees the result in one loop. `desktop_studio/mcp/hws.py` is a shell wrapper for agents without MCP registration.

The agent instructions are a skill file: [`desktop_studio/docs/agents/SKILL.md`](desktop_studio/docs/agents/SKILL.md) (also `.claude/skills/hairwork-studio/SKILL.md` in the repository, so Claude Code loads it as the `hairwork-studio` skill, and [`AGENTS.md`](AGENTS.md) points to it). The deeper reference is [`desktop_studio/docs/AGENT_GUIDE.md`](desktop_studio/docs/AGENT_GUIDE.md).

**Register the server** (`<studio>` = the unzipped release; from a clone built from source use `py` instead of the bundled python):

```
claude mcp add hairwork-studio -- <studio>\python\python.exe <studio>\desktop_studio\mcp\hairwork_studio_mcp.py
```

Any other MCP client (stdio):

```json
{
  "mcpServers": {
    "hairwork-studio": {
      "command": "<studio>\\python\\python.exe",
      "args": ["<studio>\\desktop_studio\\mcp\\hairwork_studio_mcp.py"],
      "env": { "HWFA_GAME": "<your Witcher 3 folder, only if the Studio does not find it>" }
    }
  }
}
```

Environment: `HAIRWORK_STUDIO_EXE` (exe path override), `HWS_OUT` (output folder, default `<studio>\build\desktop_studio\mcp`), `HWFA_GAME` (game folder), `HWS_SESSION` / `HWS_PORT` (parallel sessions).

### Starter prompt

Paste this into your coding agent (Claude Code, etc.) in an empty folder. It downloads the Studio, clones the repositories, registers the MCP and starts working with you:

```text
I want to make HairWorks hair for The Witcher 3 (next-gen 5.0, DX12) with Hairwork Studio. Please set everything up and then work with me:

1. Download the latest Hairwork Studio release: `gh release download --repo jlrouzies-fr/Witcher3-HairworkStudio --pattern "*.zip" --dir C:\HairworkStudio_dl` (or from https://github.com/jlrouzies-fr/Witcher3-HairworkStudio/releases/latest) and unzip it into `C:\HairworkStudio` (a folder I can write to, not Program Files). The zip contains one folder `HairworkStudio-<version>`; below, <studio> means that folder, e.g. `C:\HairworkStudio\HairworkStudio-0.2.1`. Also `git clone https://github.com/jlrouzies-fr/Witcher3-HairworkStudio` next to it for the docs and source.
2. Clone the community hair packs: `git clone https://github.com/jlrouzies-fr/Witcher3-HairworkCommunity`.
3. Ask me for the folder of my Witcher 3 install (patch 5.0, the folder that contains content\content0 and mods) and set the environment variable HWFA_GAME to it if the Studio does not find it by itself. Never copy or upload any game file.
4. Register the Studio MCP server: `claude mcp add hairwork-studio -- <studio>\python\python.exe <studio>\desktop_studio\mcp\hairwork_studio_mcp.py` (absolute paths), then tell me to restart this session if the studio_* tools are not visible yet.
5. Read <studio>\docs\agents\SKILL.md (the hairwork-studio skill; in the git clone it is desktop_studio\docs\agents\SKILL.md) completely and follow its rules.
6. Then start: list the hair builds with studio_list_configs, ask me which character I want, studio_open it (the first open builds the strand pack from my game files and takes a few minutes), show me studio_compare against the vanilla hair, and edit with me: render, fix one visible defect at a time, keep the hair budget under 1.2, save under a new name, and finally export a mod (and, if I want to share it, a hair pack for the community repo).
```

The Studio needs your own copy of The Witcher 3 (patch 5.0, DX12): it builds the strand packs from your game files at run time and ships none. Mods exported with "Export for release" need Hairwork for All - Core (Nexus 13996) and Graphics Control (Nexus 13349).

## Contributing

Pull requests are welcome under the Hairwork Studio License (see section 1.3 of the licence). Run
`git config core.hooksPath .githooks` once per clone so the SDK guard runs before every commit; never add NVIDIA SDK files.
