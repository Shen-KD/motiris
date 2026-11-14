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
make VISION_TOOLS=1   # opt in to host-backed vision_analyze
make test-vision      # clean optional build + bounded local-provider tests
make TTS_TOOLS=1      # opt in to host-only REPL external TTS adapter
make test-tts         # clean optional build + fake-engine integration tests
```

### Platform matrix and file-search resource bounds

| Target | Build/test evidence | Boundary |
| --- | --- | --- |
| Linux gcc / clang | `make`, `make test`, CI `-Werror` and ASAN/UBSAN | Full host CLI; libcurl and POSIX tools enabled |
| Host CLI without file backend | `make test-nofs` (`FILE_TOOLS=0`) | Excludes `filetools.c`; `filetools_nofs.c` returns a fixed bounded error for all four file tools. Other host tools/harness are unchanged |
| Optional host vision | `make test-vision` (`VISION_TOOLS=1`) | One workspace-local image to a configured remote provider; separate POSIX file + libcurl backend, excluded by default and not local inference |
| Optional host TTS | `make test-tts` (`TTS_TOOLS=1`) | Final REPL text to one trusted external executable over a bounded pipe; fake-engine tests only, no audio device or MCU claim |
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

### Optional vision bounds and measurements

`VISION_TOOLS=1` compiles `src/tools/visiontools.c` and three agent getters;
`VISION_TOOLS=0` filters out the source and all conditional core references.
The tool has only per-call state and a per-agent context pointer. It does not
retain image bytes, start threads, run inference, spawn processes or add a
runtime dependency beyond the existing host libcurl/POSIX backend. Its file
backend is independent of `FILE_TOOLS`; no RTOS/HAL input adapter is claimed.

For an input of `n` bytes, configuration enforces `1 <= n <= 1,048,576`
(default limit 262,144). Base64 length is `b = 4 * ceil(n / 3)`. The encoding
phase owns at most `n + b` image bytes. The raw input is then freed; the data
URL is referenced (not duplicated) by cJSON, so request serialization owns
about `2 * b` plus a question capped at 4 KiB and a fixed set of JSON nodes.
Network receive storage is capped at 64 KiB. Parsed provider text is bounded
by that response, the returned copy is capped at 16 KiB, and worst-case JSON
escaping of that copy is below 96 KiB. These are phase-wise application-owned
bounds, not a claim about libcurl/libc internals or the outer agent history.
The total timeout is configurable from 1 to 120 s (default 30 s); redirects
are disabled and there is no background/idle network activity.

Reference host measurements on 2026-11-15 (Linux x86_64, gcc `-O2`, stripped;
baseline `0cfb043`) are:

| Build / measurement | Bytes / RSS | Relative result |
| --- | ---: | ---: |
| Default full CLI | 157,944 bytes; text 144,405; data 2,648; BSS 167,960 | baseline; vision source/references compiled out |
| `VISION_TOOLS=1` full CLI | 166,136 bytes; text 153,541; data 2,656; BSS 167,960 | +8,192 total; +9,136 text; +8 data; BSS unchanged |
| Maximum 1 MiB image through local mock | 14,320 KiB peak process RSS | Includes outer agent, libcurl, mock round-trip and serialization; not an MCU RAM budget |

`tests/vision.sh` verifies the exact data URL bytes/MIME and content blocks,
invalid MIME, configurable and hard input caps, workspace escape refusal,
disconnected/HTTP providers, response/description caps and the exact 1 MiB
round-trip. No MCU cross-toolchain, target Flash/RAM or hardware validation
is available; all numbers above are host-only.

### Optional host-only TTS bounds and measurements

`TTS_TOOLS=1` compiles `src/harness/tts.c` and the conditional REPL call site.
It adds no public core API and no agent/session state. After a successful turn,
the REPL selects the new final assistant message (skipping goal-check's
synthetic `DONE`) and passes it to `MOTIRIS_TTS_CMD` with `exec`, not a shell.
The executable owns synthesis, playback and audio storage; motiris never
receives audio and discards engine stdout/stderr.

Each call reads at most 16,384 bytes from the existing reply string. The parent
adds no heap text copy or audio buffer, retains nothing between turns, and owns
only one nonblocking pipe plus one child process. Writes and exit waiting share
one monotonic deadline (`MOTIRIS_TTS_TIMEOUT_S`, default 10 s, hard range
1..120 s). Expiry kills the child process group and reaps the direct child.
Early EOF, `SIGPIPE`, nonzero exit, missing executables and invalid settings
all leave the already printed text intact with a bounded diagnostic. Unset
`MOTIRIS_TTS_CMD` means no process or idle background activity. Real engine
CPU/RSS/audio limits remain backend responsibilities and are not MCU budgets.

Reference host measurements on 2026-11-15 (Linux x86_64, gcc `-O2`, stripped;
baseline `65f6584`) are:

| Build / measurement | Bytes / sections | Relative result |
| --- | ---: | ---: |
| Default full CLI | 157,944 bytes; text 144,405; data 2,648; BSS 167,960 | baseline; TTS source and REPL references compiled out |
| `TTS_TOOLS=1` full CLI | 162,040 bytes; text 148,801; data 2,680; BSS 167,960 | +4,096 file; +4,396 text; +32 data; BSS unchanged |
| Optional REPL + `/bin/sh` fake sink | 9,112 KiB peak process RSS | One echo reply through the bounded pipe; no real audio engine/device |
| Default `tests/perf.sh` cold / 5-step loop peak RSS | 8,924 / 8,984 KiB | baseline 8,992 / 9,008 KiB; both below 64 MiB guard |

Default file/text/data/BSS deltas are all zero; default and optional direct
ELF dependencies remain only `libcurl.so.4` and `libc.so.6`. The core source and public header
are untouched. These are host binary/RSS measurements, not target Flash/RAM
headroom. `tests/tts.sh` covers eight fake-engine cases: exact text, backend
failure, timeout/reaping, truncation, goal-check filtering, missing engine,
invalid timeout and runtime-disabled behavior. No real speech engine, audio
device, MCU cross-toolchain or hardware validation is claimed.

CI (`.github/workflows/ci.yml`) is split into per-area jobs, all gated
by the aggregate `ci` job (that exact name is the required status check
on main): `build` (gcc clean compile+link, size guard, artifact),
`clang` (full -Werror build), `test-core` / `test-tools` / `test-repl` /
`test-skills` / `test-schema` / `test-goal` (component-scoped smoke
groups, see `tests/smoke.sh <group>`), `plugins` (example .so with gcc +
clang -Werror), `tools` (component static libs compile independently
with clang -Werror), `no-filesystem` (strict-C11 core and optional file
backend cut), `vision` (optional local-mock integration and clang `-Werror`),
`tts` (optional fake-engine integration and clang `-Werror`),
and `sanitizers` (ASAN/UBSAN tools smoke).
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

Optional vision: `VISION_TOOLS=1` registers `vision_analyze` through the same
per-agent registry in CLI, REPL, gateway and cron builds. The optional getters
`motiris_base_url` / `motiris_api_key` / `motiris_transport_name` expose the
selected default provider without making the agent struct public. Vision-specific
`MOTIRIS_VISION_*` overrides are read per call; no global session state is added.

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
