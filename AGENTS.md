# AGENTS.md: Hairwork Studio

Agents working with Hairwork Studio (the HairWorks hair editor for The Witcher 3): read [`desktop_studio/docs/agents/SKILL.md`](desktop_studio/docs/agents/SKILL.md) first (Claude Code loads it as the skill `hairwork-studio` from `.claude/skills/hairwork-studio/`). It covers setup, the MCP server, the editing loop, hair budget and path-tracing limits, and export / sharing. Deeper reference: [`desktop_studio/docs/AGENT_GUIDE.md`](desktop_studio/docs/AGENT_GUIDE.md); setup and starter prompt: the "Agents / MCP" section of [`README.md`](README.md).

Rules that never change:

- Never commit or upload NVIDIA HairWorks SDK material (headers, sources, docs, samples, Viewer). Run `git config core.hooksPath .githooks` once per clone; `py tools/sdk_guard.py --tracked` checks the tree.
- Never ship game files, third-party mods or their files. Hair packs are data only.
- Save edits and exports under new names; do not overwrite builds you did not make.
- Do not drive the Studio with real mouse / keyboard input; use the MCP or the `--agent` runner.
- Never put the user's e-mail, tokens or API keys into the repository.
