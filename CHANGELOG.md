# Changelog

All notable changes to motiris. Format keeps it terse: version, date,
breaking notes, then the highlights. Releases are tagged `vX.Y.Z` and
published by `.github/workflows/release.yml`.

## [0.1.0] - 2026-09-20

First tagged release. A featherweight C11 agent framework: one binary
(~133 KB, libcurl.so.4 as the only runtime dep).

### Added

- Agent loop with tool calling over OpenAI-compatible chat
  completions; SSE streaming; provider fallback list (config
  `providers`).
- Tools (built-in): shell (allow/deny prefix policy), time,
  file read/write/patch/search (workspace-guarded), web_fetch /
  web_search (keyless DDG), persistent memory, SKILL.md index +
  on-demand load, subagent (child `motiris` process), browser_fetch
  (headless chromium).
- MCP client: stdio JSON-RPC servers via config `mcp_servers`;
  remote tools registered as `<server>:<tool>`.
- HTTP gateway (`--gateway`, token auth, session-per-chat_id),
  cron jobs (`--cron job.json [--once]`), session listing
  (`--sessions`).
- Runtime plugins (.so, `motiris_plugin_init`), tool-call observers
  (hook API), cumulative token counters.
- Interactive REPL: colored output, `/-command` tab completion and
  hints, tool lines and per-turn token stats.
- Release automation: tag-driven release workflow, distro/compiler
  compat matrix (ubuntu 22.04/24.04/26.04 gcc 11/13/15,
  centos-stream9, clang), perf/resource guards, `install.sh`
  one-shot installer from GitHub releases.
- Offline smoke suite (17 checks) via mock LLM/MCP servers;
  `make test-docker` for isolated container runs.

### Fixed

- SSE stream: buffer overflow on the first delta (unterminated
  buffer before strcat) — found by real TaaS streaming.
- MCP: params double-free (cJSON ownership transfer).
- browser_fetch: use-after-free of `url` after args freed.
- glibc < 2.38 link failure (`__isoc23_strtol/sscanf`) via
  src/compat.c shims.
- REPL history file is ~/.motiris_history, writable.

### Known limits

- Prebuilt release binary: linux x86_64 only.
- browser_fetch needs a system chromium binary.
- Tool hooks and stream sink are per-agent observers, not
  persisted across processes.

[0.1.0]: https://github.com/Shen-KD/motiris/releases/tag/v0.1.0