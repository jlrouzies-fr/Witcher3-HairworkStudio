"""Shared text for the Studio release README and the public repo README / AGENTS.md (agents + MCP section, starter prompt)."""

STARTER_PROMPT = r"""I want to make HairWorks hair for The Witcher 3 (next-gen 5.0, DX12) with Hairwork Studio. Please set everything up and then work with me:

1. Download the latest Hairwork Studio release: `gh release download --repo jlrouzies-fr/Witcher3-HairworkStudio --pattern "*.zip" --dir C:\HairworkStudio_dl` (or from https://github.com/jlrouzies-fr/Witcher3-HairworkStudio/releases/latest) and unzip it into `C:\HairworkStudio` (a folder I can write to, not Program Files). The zip contains one folder `HairworkStudio-<version>`; below, <studio> means that folder, e.g. `C:\HairworkStudio\HairworkStudio-0.2.1`. Also `git clone https://github.com/jlrouzies-fr/Witcher3-HairworkStudio` next to it for the docs and source.
2. Clone the community hair packs: `git clone https://github.com/jlrouzies-fr/Witcher3-HairworkCommunity`.
3. Ask me for the folder of my Witcher 3 install (patch 5.0, the folder that contains content\content0 and mods) and set the environment variable HWFA_GAME to it if the Studio does not find it by itself. Never copy or upload any game file.
4. Register the Studio MCP server: `claude mcp add hairwork-studio -- <studio>\python\python.exe <studio>\desktop_studio\mcp\hairwork_studio_mcp.py` (absolute paths), then tell me to restart this session if the studio_* tools are not visible yet.
5. Read <studio>\docs\agents\SKILL.md (the hairwork-studio skill; in the git clone it is desktop_studio\docs\agents\SKILL.md) completely and follow its rules.
6. Then start: list the hair builds with studio_list_configs, ask me which character I want, studio_open it (the first open builds the strand pack from my game files and takes a few minutes), show me studio_compare against the vanilla hair, and edit with me: render, fix one visible defect at a time, keep the hair budget under 1.2, save under a new name, and finally export a mod (and, if I want to share it, a hair pack for the community repo).
"""

AGENTS_MD_BODY = r"""## Agents / MCP

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
@@PROMPT@@```

The Studio needs your own copy of The Witcher 3 (patch 5.0, DX12): it builds the strand packs from your game files at run time and ships none. Mods exported with "Export for release" need Hairwork for All - Core (Nexus 13996) and Graphics Control (Nexus 13349).
"""

AGENTS_ROOT_MD = r"""# AGENTS.md: Hairwork Studio

Agents working with Hairwork Studio (the HairWorks hair editor for The Witcher 3): read [`desktop_studio/docs/agents/SKILL.md`](desktop_studio/docs/agents/SKILL.md) first (Claude Code loads it as the skill `hairwork-studio` from `.claude/skills/hairwork-studio/`). It covers setup, the MCP server, the editing loop, hair budget and path-tracing limits, and export / sharing. Deeper reference: [`desktop_studio/docs/AGENT_GUIDE.md`](desktop_studio/docs/AGENT_GUIDE.md); setup and starter prompt: the "Agents / MCP" section of [`README.md`](README.md).

Rules that never change:

- Never commit or upload NVIDIA HairWorks SDK material (headers, sources, docs, samples, Viewer). Run `git config core.hooksPath .githooks` once per clone; `py tools/sdk_guard.py --tracked` checks the tree.
- Never ship game files, third-party mods or their files. Hair packs are data only.
- Save edits and exports under new names; do not overwrite builds you did not make.
- Do not drive the Studio with real mouse / keyboard input; use the MCP or the `--agent` runner.
- Never put the user's e-mail, tokens or API keys into the repository.
"""


def agents_md():
    return AGENTS_MD_BODY.replace('@@PROMPT@@', STARTER_PROMPT)


def agents_txt():
    """plain-text version for the release README.txt"""
    L = [r'AGENTS / MCP (let a coding agent drive the Studio)',
         r'The Studio ships an MCP server: desktop_studio\mcp\hairwork_studio_mcp.py (runs on the bundled python\python.exe, no',
         r'install). It runs a hidden live Studio session and exposes the editing tools; render tools return the picture, so an',
         r'agent (Claude Code or any MCP client) edits and sees the result. Agent instructions: docs\agents\SKILL.md; deeper',
         r'reference: docs\AGENT_GUIDE.md. Register (replace <studio> by this folder):',
         r'  claude mcp add hairwork-studio -- <studio>\python\python.exe <studio>\desktop_studio\mcp\hairwork_studio_mcp.py',
         r'Other MCP clients: command = <studio>\python\python.exe, args = [<studio>\desktop_studio\mcp\hairwork_studio_mcp.py].',
         r'Starter prompt to paste into your agent (it downloads the Studio, clones the repos, registers the MCP, starts):', '']
    L += ['    ' + l if l else '' for l in STARTER_PROMPT.split('\n')]
    return '\n'.join(L) + '\n'
