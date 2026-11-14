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
make test             # offline smoke suite (27 checks, mock LLM/MCP)
make test-docker      # same suite in a throwaway debian container
sh tests/perf.sh      # cold-start latency, peak RSS, agent-loop guard
make FILE_TOOLS=0     # optional POSIX file backend -> bounded stubs
make test-nofs        # strict-C11 core probe + full-CLI backend-cut test
```

### Platform matrix and file-search resource bounds

| Target | Build/test evidence | Boundary |
| --- | --- | --- |
| Linux gcc / clang | `make`, `make test`, CI `-Werror` and ASAN/UBSAN | Full host CLI; libcurl and POSIX tools enabled |
| Host CLI without file backend | `make test-nofs` (`FILE_TOOLS=0`) | Excludes `filetools.c`; `filetools_nofs.c` returns a fixed bounded error for all four file tools. Other host tools/harness are unchanged |
| Portable core-only probe | `make test-core-nofs` | Strict C11 `-Werror`; core loop/config/echo transport + cJSON, no POSIX feature macros, filesystem I/O or libcurl. A small test adapter supplies tool/schema registries; this is not the full CLI |
| MCU / RTOS | **待定 / 未验证** | Chip, RTOS, cross-toolchain, RAM/Flash and hardware test matrix have not been supplied; host results are not MCU support claims |

`search_files` keeps directory enumeration inside the optional POSIX file
backend. An explicitly named start path is canonicalized and checked against
the workspace, then reopened relative to a workspace directory fd. Every
component and recursive entry uses `openat(..., O_NOFOLLOW)`; recursive
symlinks (including loops) are skipped. This closes symlink replacement
between a pathname check and open without adding any dependency to core.
Read/write/patch keep their existing canonical-path behavior.

Traversal is iterative, not recursive in C. Its hard limits are 32 open
directory levels, 4096 visited entries (excluding `.`/`..`), 1 MiB read per
file, 16 MiB read per call, `PATH_MAX` bytes per path, a 4096-byte line buffer
and 200 matches. Overlong lines are drained and skipped. With
`PATH_MAX=4096`, active/transient path strings (including the canonical start
path) occupy at most 34 * 4096 = 136 KiB; the fixed line buffer is 4 KiB and
the fixed 32-frame stack is below 1 KiB
(on the current 64-bit host). Raw match path+text storage is below
`200 * (4095 + 4095)` = 1,638,000 bytes, excluding bounded cJSON node
overhead. Worst-case JSON escaping keeps the printed result below roughly
10 MiB on that host. libc regex/DIR allocation is implementation-dependent,
but directory recursion and application-owned storage have the bounds above.
Diagnostics report node/byte counts, skipped links, overlong lines,
unreadable entries and truncation reasons.

Reference host measurements on 2026-11-15 (Linux x86_64, gcc `-O2`, stripped
unless noted; baseline `e315297`) are:

| Build / measurement | Bytes / RSS | Relative result |
| --- | ---: | ---: |
| Baseline full CLI | 153,848 bytes; text 140,594; data 2,584; BSS 167,960 | baseline |
| Bounded-search full CLI | 157,944 bytes; text 144,405; data 2,648; BSS 167,960 | +4,096 total; +3,811 text; +64 data; BSS unchanged |
| `FILE_TOOLS=0` host CLI | 145,656 bytes; text 134,899; data 2,536; BSS 163,864 | optional backend/stub delta visible; no new core dependency |
| Core-only nofs probe | 52,264 bytes unstripped; text 34,991; data 800; BSS 4,224 | links libc only, no libcurl |
| `tests/perf.sh` cold / 5-step loop peak RSS | 9,004 / 8,856 KiB | baseline 8,940 / 8,872 KiB; both below 64 MiB guard |
| Search stress max child peak RSS | 29,424 KiB | Linux `ru_maxrss`; includes long-path + 200-result provider round |

These are relative host measurements, not absolute MCU RAM/Flash headroom.
The no-filesystem switch does not make unrelated host tools, HTTP transports,
plugins, sessions or gateway MCU-portable. A real target port still needs its
own HAL/RTOS backends and measured RAM/Flash budget.

CI (`.github/workflows/ci.yml`) is split into per-area jobs, all gated
by the aggregate `ci` job (that exact name is the required status check
on main): `build` (gcc clean compile+link, size guard, artifact),
`clang` (full -Werror build), `test-core` / `test-tools` / `test-repl` /
`test-skills` / `test-schema` / `test-goal` (component-scoped smoke
groups, see `tests/smoke.sh <group>`), `plugins` (example .so with gcc +
clang -Werror), `tools` (component static libs compile independently
with clang -Werror), `no-filesystem` (strict-C11 core and optional file
backend cut), and `sanitizers` (ASAN/UBSAN tools smoke).
`perf` and `compat-matrix` are informational
(ubuntu 22.04 gcc-11 / 24.04 gcc-13 / 26.04 gcc-15 / centos-stream9
container / clang).

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

Sessions: `src/harness/sessions.c` owns the shared U/A/T log helpers
(`motiris_state_dir` / `motiris_session_list` / `motiris_session_append`);
`--sessions`, the gateway and the repl (/sessions) all go through it.

Skill files: `skill_patch` / `skill_write` in skilltools.c edit the
indexed `--skill-dir` files in place; paths are built from the index
(`skill_dir` + `file`), never from user input — no workspace guard
needed (by construction bounded to the skill dir).

Per-tool schemas: `src/tools/toolscan.c` indexes
`<plugin_dir>/<name>/<name>.json` and `<tools_dir>/<name>/<name>.json`
(plugin dir = MOTIRIS_PLUGIN_DIR or ~/.motiris/plugins; tools dir =
MOTIRIS_TOOLS_DIR or ~/.motiris/tools). `motiris_register_tool` swaps
in the external schema when one exists. `motiris_plugin_dir_default()`
lives in plugin.c and is shared with toolscan.

## Tests that must not regress

- Smoke #4: binary stays < 163840 bytes.
- Smoke #9: gateway health/auth plus partial, byte-segmented, malformed
  and oversized requests. Headers are capped at 16 KiB and bodies at
  1 MiB; the receive allocation is capped at their sum plus one NUL byte.
  Gateway session regressions cover colliding punctuation, case, Unicode,
  120-byte IDs, restart and 64-session eviction replay, fail-closed identity
  headers, and exact-ID reset without cross-session deletion.
- Smoke #14: SSE delta aggregation (first-delta termination).
- Smoke #16: repl shows tool lines + token stats (streamed tool_calls
  + usage through the mock).
- Smoke #17: plugin tool hook fires (needs examples/hello_plugin.so).
- Smoke #20/#21: skill_patch / skill_write edit real skill files via
  the agent loop (mock model), matching the current schema strings —
  if you change a tool schema, update these mocks.
- Smoke #24-26: per-tool schema files (override / invalid json / plugin
  subdir layout) — if you change the merge logic or dir resolution,
  update these.
- Smoke #27: goal-check loop (`--goal-check` + DONE confirmation).
- Goal-check state is per-agent (goal from the agent's own messages,
  `confirm_asked` is a `motiris_run` local); no new global state — keep
  it that way when touch agent.c.
