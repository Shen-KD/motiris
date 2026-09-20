# DEVELOPING motiris

Field notes for working on this repo: layout, build/test loop,
extension points, and the traps that have already burned time.

## Architecture

Single C11 binary, zero runtime deps beyond libcurl.so.4. Components
are static archives linked into `motiris`:

```
src/core/     agent loop, config, transports        -> libmotiris_core.a
src/tools/    built-in tool bundles                 -> libmotiris_tools.a
src/harness/  repl, gateway, cron, plugins, mcp     -> libmotiris_harness.a
src/vendor/   vendored cJSON (MIT)
deps/         linenoise (line editing), libcurl dev headers (curl 8.18 ABI)
```

- `include/motiris.h` is the ONLY public header; tools/plugins see it.
  Keep declarations grouped; `MotirisToolHook` must be typedef'd before
  `MotirisPluginApi` uses it (declaration order — burned once).
- `.DEFAULT_GOAL := motiris` in the Makefile; the first *pattern* rule
  in the file becomes the default target otherwise (burned once).
- New source files: put them in the matching component dir; `$(wildcard
  src/<dir>/*.c)` picks them up automatically. No Makefile edit needed.

## Build & test loop

```
make                  # compiles per-component .o -> .a -> motiris
make test             # offline smoke suite (17 checks, mock LLM/MCP)
make test-docker      # same suite in a throwaway debian container
sh tests/perf.sh      # cold-start latency, peak RSS, agent-loop guard
```

CI (`.github/workflows/ci.yml`) runs: gcc build, smoke, clang -Werror
build, plugin example compile, binary size (<163840), then `perf` and
the `compat-matrix` job (ubuntu 22.04 gcc-11 / 24.04 gcc-13 / 26.04
gcc-15 / centos-stream9 container / clang). `ci` is the required check
on main, the others are informational.

## Release & install

- Tag `vX.Y.Z` -> `.github/workflows/release.yml` builds, runs smoke,
  packages `motiris-<v>-linux-x86_64.tar.gz` + `motiris.sha256` +
  `motiris-info.txt`, creates the GitHub release.
- `install.sh` fetches the latest release, verifies sha256, installs,
  bootstraps `~/.motiris/{env,config.json}` (never overwrites).
- `--version` prints `MOTIRIS_VERSION` (git describe, fallback 0.1.0),
  git SHA and build date — injected via CPPFLAGS in the Makefile so CFLAGS
  overrides (CI -Werror) don't drop them.
## Working workflow (main is PR-only)

1. `git checkout -b feat/<name>` from an up-to-date main.
2. Code, keeping the smoke suite green: `make && sh tests/smoke.sh`.
3. Commit, push, `gh pr create`. CI runs 8 checks; watch `compat-matrix`
   for old-glibc/gcc regressions.
4. Merge with the admin helper (bypasses the 1-review requirement
   legitimately — solo repo):
   `bash .github/scripts/admin-merge.sh <PR>`
   which re-applies branch protection afterwards.
5. `git checkout main && git pull --rebase`.
6. If the merge changes the binary: `make install PREFIX=$HOME`
   (installs to ~/bin/motiris, the dev machine's live copy).

Never `git add -A` blindly after a build: it sweeps .o/.a artifacts
into the index. `.gitignore` covers `*.o`/`*.a` now, but previously
tracked artifacts stay tracked — check `git ls-files | grep -E
'\.(o|so|a)$'` after any refactor that moves files. Line endings in
.gitignore (and the Makefile) have been silently mangled by an editor
before — re-read the file after mechanical edits.

## Extension points

Compile-time tools (see `examples/hello_plugin.c` for a full pattern):

```c
MotirisTool t = { .name, .description, .parameters, .call, .ud };
motiris_register_tool(a, &t);     /* shallow copy — keep name/desc alive */
motiris_unregister_tool(a, name);
```

Runtime plugins: a .so exporting `motiris_plugin_init(MotirisAgent*,
const MotirisPluginApi*)`. The api currently offers
`register_tool` / `unregister_tool` / `add_tool_hook`
(tool-call observers; multi-slot, appended — the repl's hook and
plugin hooks coexist).

Agent callbacks:

- `motiris_set_stream_cb` — per-token sink (single slot; repl owns it).
- `motiris_set_tool_hook` / `motiris_add_tool_hook` — called after each
  tool executes with (name, args, result_json, ud); result may contain
  `"error"` — the repl renders that as a red ✗ line.
- `motiris_tokens_in/out` — cumulative usage across all turns of an
  agent (parsed from `usage` in every response, streamed or not).

MCP: `"mcp_servers": [{"name","cmd","args":[],"env":{}}]` in
config.json spawns a stdio JSON-RPC server; each remote tool registers
as `<server>:<tool>`. `env` overrides are applied in the child process
before exec (not yet implemented for arbitrary vars — see mcp.c).

## Tests that must not regress

- Smoke #4: binary stays < 163840 bytes.
- Smoke #14: SSE delta aggregation (first-delta termination).
- Smoke #16: repl shows tool lines + token stats (streamed tool_calls
  + usage through the mock).
- Smoke #17: plugin tool hook fires (needs examples/hello_plugin.so).
