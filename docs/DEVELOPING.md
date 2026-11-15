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
make SHELL_TOOLS=0    # compile out and unregister host shell subprocesses
make test-shell       # dual-stream, output cap, timeout and reaping tests
make test-nofs        # strict-C11 core probe + full-CLI backend-cut test
make VISION_TOOLS=1   # opt in to host-backed vision_analyze
make test-vision      # clean optional build + bounded local-provider tests
make TTS_TOOLS=1      # opt in to host-only REPL external TTS adapter
make test-tts         # clean optional build + fake-engine integration tests
```

### Platform matrix and resource bounds

| Target | Build/test evidence | Boundary |
| --- | --- | --- |
| Linux gcc / clang | `make`, `make test`, CI `-Werror` and ASAN/UBSAN | Full host CLI; libcurl and POSIX tools enabled |
| Host CLI without file/shell backends | `make test-nofs` (`FILE_TOOLS=0 SHELL_TOOLS=0`) | Excludes `filetools.c` and the shell subprocess implementation; file tools return fixed bounded errors and `shell` is unregistered. Other host tools/harness are unchanged |
| Optional host vision | `make test-vision` (`VISION_TOOLS=1`) | One workspace-local image to a configured remote provider; separate POSIX file + libcurl backend, excluded by default and not local inference |
| Optional host TTS | `make test-tts` (`TTS_TOOLS=1`) | Final REPL text to one trusted external executable over a bounded pipe; fake-engine tests only, no audio device or MCU claim |
| Portable core-only probe | `make test-core-nofs` | Strict C11 `-Werror`; core loop/config/echo transport, bounded memory snapshot codec + cJSON, no POSIX feature macros, filesystem I/O or libcurl. A small test adapter supplies tool/schema registries; this is not the full CLI |
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

### Bounded optional shell subprocess backend

`SHELL_TOOLS=1` is the host default; `SHELL_TOOLS=0` defines
`MOTIRIS_NO_SHELL`, compiling the `fork`/`pipe`/`poll`/`waitpid` implementation
and its registry entry out of `src/tools/tools.c`. The strict core-only probe
does not compile this host tool at all. No public API, agent/session state,
thread, third-party runtime or direct dependency is added.

The parent creates a process group and uses nonblocking stdout/stderr fds with
one monotonic deadline. It interleaves reads instead of waiting for either
stream's EOF first. Bytes beyond either per-stream cap or the shared total cap
are drained and discarded, with `truncated`, `stdout_truncated` and
`stderr_truncated` diagnostics. Timeout, setup/poll/read/wait errors kill the
process group and reap the direct child; all pipe/fork/fcntl failure paths
close their fds. Results retain `exit_code`, add `signal` and `timed_out`, and
return bounded model-visible errors. This is execution budgeting, not a
sandbox, general cancellation tree or asynchronous agent runtime.

Defaults are 30 s, 16 KiB per stream and 32 KiB total. Environment overrides
are `MOTIRIS_SHELL_TIMEOUT_S` (1..120 s), `MOTIRIS_SHELL_STDOUT_MAX`,
`MOTIRIS_SHELL_STDERR_MAX` and `MOTIRIS_SHELL_OUTPUT_MAX` (each output setting
1..262,144 bytes). Invalid values fail before process creation. Let `T` be
the total output cap. Exact-size reallocations keep retained stdout+stderr at
at most `T` bytes plus two NUL bytes; the read scratch buffer is 4 KiB. cJSON
holds another at-most-`T` copy while constructing the result. Worst-case
control-byte escaping makes the returned JSON and its agent-history copy each
smaller than `6*T + 512` bytes (about 1.5 MiB at the hard cap). Request
construction creates similarly bounded duplicate/serialized copies. Raw
stream buffers are freed before the handler returns; only the bounded final
tool result remains in the existing conversation history. These are per-call
application bounds, not a new total-session history policy or a libc/libcurl
internal allocation guarantee.

Reference host measurements on 2026-11-15 (Linux x86_64, gcc `-O2`, stripped;
baseline `1af30f9`) are:

| Build / measurement | Bytes / sections / RSS | Relative result |
| --- | ---: | ---: |
| Baseline default full CLI | 157,944 bytes; text 144,973; data 2,648; BSS 167,960 | baseline |
| Bounded-shell default full CLI | 162,040 bytes; text 148,549; data 2,664; BSS 167,960 | +4,096 file; +3,576 text; +16 data; BSS unchanged; below 163,840-byte guard |
| `SHELL_TOOLS=0` full CLI | 153,848 bytes; text 143,165; data 2,616; BSS 167,960 | optional backend and registration compiled out |
| Default `tests/perf.sh` cold / 5-step loop peak RSS | 8,996 / 9,324 KiB | baseline 9,136 / 9,188 KiB; both below 64 MiB guard |
| Maximum 256 KiB cap through local mock | 21,476 KiB max child RSS; Agent RSS 11,816 -> 12,508 KiB | +692 KiB retained across the sampled call; host allocator/libcurl included |
| Timeout regression | 1.008 s for a 1 s deadline | background `sleep` descendant confirmed gone |

Default and shell-cut direct ELF dependencies remain only `libcurl.so.4` and
`libc.so.6`; `src/core/` and the public header are unchanged. `tests/shell.py`
covers short/exit-status behavior, the 128 KiB stderr-first deadlock,
alternating dual-stream output, exact/total caps, invalid settings, timeout
process-group cleanup, 16 repeated calls without fd/direct-child leaks, and
the maximum cap/result/RSS bounds. It runs in smoke #11, including the
ASAN/UBSAN tools job. No MCU/RTOS cross-toolchain, target Flash/RAM or
hardware validation is available; none of these host numbers is an MCU claim.

### Bounded web tool responses

`web_fetch` and `web_search` share one per-GET receive buffer. It retains at
most 65,536 response bytes and requests at most 65,537 bytes including the
NUL terminator. Growth covers the current chunk before copying, multiplication
and addition stay within that bound, and a temporary pointer preserves the
original allocation if `realloc` fails. Overflow/OOM returns a bounded tool
error and frees the original buffer. After the cap, the callback reports bytes
as consumed but discards them, so libcurl drains the response without growing
the buffer or turning normal oversized pages into a write error. Both tools
add `"truncated": true` when bytes were discarded; successful empty bodies
return an empty string. Tool input schemas and public APIs are unchanged.

The buffer is per-call state, not per-agent global or retained session state.
HTML stripping is in place. cJSON result construction/serialization adds
phase-wise allocations derived from the same cap, while libcurl/libc internals
remain implementation-dependent. No thread, process, runtime or default core
dependency was added. The host web tools are absent from the strict core-only
nofs probe; `FILE_TOOLS=0` behavior is unchanged.

Reference host measurements on 2026-11-15 (Linux x86_64, gcc `-O2`, stripped;
baseline `85502b9`) are:

| Build / measurement | Bytes / RSS | Relative result |
| --- | ---: | ---: |
| Baseline default full CLI | 157,944 bytes; text 144,405; data 2,648; BSS 167,960 | baseline |
| Fixed default full CLI | 157,944 bytes; text 144,973; data 2,648; BSS 167,960 | +0 file; +568 text; data/BSS unchanged |
| `tests/perf.sh` cold / 5-step loop peak RSS | 9,016 / 9,016 KiB | baseline 9,028 / 9,148 KiB; normal host variation |
| `tests/webtools.py`, including 1 MiB response | 21,940 KiB max child RSS | includes full agent/provider/libcurl rounds, not just the 65,537-byte receive allocation |

The integration test drives actual model tool-calls and local GETs through
16 KiB±1, byte/irregular segmentation, 64 KiB±1, empty, 128 KiB and 1 MiB
responses. It runs in smoke #11, including the ASAN/UBSAN tools job. These are
host bounds and measurements only; no MCU/RTOS cross-toolchain, target
Flash/RAM or hardware validation is available.

### Bounded live history, replay and full-turn checkpoints

`src/core/agent.c` owns per-agent live-history and replay budgets; there is no
global session state. Both default to 256 messages and 16 MiB measured as the
sum of compact serialized message records. `MOTIRIS_HISTORY_MAX_MESSAGES` /
`MOTIRIS_HISTORY_MAX_BYTES` and `MOTIRIS_REPLAY_MAX_MESSAGES` /
`MOTIRIS_REPLAY_MAX_BYTES` configure the CLI, while
`motiris_set_history_limits` / `motiris_set_replay_limits` expose the same
bounds to embedders. Values above 256 or 16 MiB and zero are rejected. The
16 MiB ceiling accommodates existing bounded host-tool results; it is not
preallocated and is not a recommended MCU budget. A target port must choose
and measure a smaller appropriate limit.

Every append accounts for the candidate and current records before mutation.
Overflow evicts oldest complete user turns only. The active turn (including
its assistant calls and ordered tool results) is protected; if it cannot fit,
the run fails without truncating content, splitting a tool sequence or sending
an over-budget request. Serialized measurement uses one bounded temporary
record at a time; cJSON trees, request duplicates and printed request bodies
add phase-wise copies, so the byte metric is not a total-process RSS guarantee.

Replay independently selects the whole active turn plus the newest complete
older turns that fit. It never deletes live/checkpoint state and fails before
transport if the active turn alone exceeds replay limits. Checkpoint restore
calls `motiris_history_retain`, so a smaller live budget is enforced before the
REPL/CLI reports its retained message count. Verbose mode diagnoses both live
eviction and replay omission. The strict-C11 nofs probe exercises two complete
turns with a four-message live/replay limit and no POSIX/file/libcurl backend.

`src/core/checkpoint.c` owns the versioned snapshot contract and the portable
`motiris_session_export` / `motiris_session_import` memory-buffer API. Export
returns a malloc'd NUL-terminated buffer and explicit length; the caller owns
it and the core retains no exported copy. Import requires a readable NUL at
`buffer[length]`, validates before restore and applies live retention. The
codec uses existing cJSON plus bounded dynamic allocation, with no global
session state, database, thread, subprocess, socket, TLS, filesystem or new
runtime dependency. The strict-C11 nofs probe exercises exact full-turn
round-trip plus ownership and corruption rejection without host file I/O.

`src/harness/checkpoints.c` is the optional host stdio backend for CLI
`-r/--save`, REPL `/resume`/interactive turns, and gateway restart/eviction.
It delegates the same snapshot contract to the core codec and adds only
bounded reads, mode-0600 writes, flush/`fsync` and atomic `rename`. The host
backend is not linked into `build/nofs/core-nofs-test`. Caller-owned memory
snapshots are volatile unless an embedder supplies its own bounded storage;
they are not a durable RTOS/HAL implementation by themselves.

The versioned `motiris-full-turn` JSON document stores `session_id`, complete
ordered `messages` and a digest. Memory export/import and host-file save/load
use identical format, limits and validation. Only user/assistant/tool roles
and the `role`, `content`, `tool_calls`, `tool_call_id` message fields are
accepted.
Function calls and results must match exactly in order, the first message must
be user and the last must be assistant. Restore requires an empty context
(`/new` before REPL `/resume` when needed), validates ownership and never
executes an old tool. The FNV-1a-64 digest detects accidental corruption; it
is not a cryptographic signature or protection against an editor who can also
recompute it. File permissions and caller/path trust remain necessary.

Hard limits are 1 MiB per snapshot read/write, 256 messages, 256 total tool
calls and 256 KiB per serialized message. The read buffer is capped at
1 MiB + 2 bytes; cJSON node overhead is implementation-dependent but the input
and node count are byte-bounded. Memory export returns at most 1 MiB plus its
NUL and retains no core-owned copy afterward; import accepts at most 1 MiB
and 256 records, then applies the configured live-history limits. Save/export
and load/import temporarily hold bounded per-record, message-payload,
duplicated-tree and root-serialization copies. Exact cJSON/libc overhead is
implementation-dependent, but there is no idle or background storage cost.
The previous valid file is preserved on validation/write failure: a short
same-directory
mode-0600 temporary file is fully written, flushed/`fsync`ed and atomically
renamed. The short temporary basename also fits a maximum-length 244-byte
gateway filename under `NAME_MAX`. Only one latest snapshot is kept per
session; failed turns are not saved. Gateway responses distinguish `saved`,
`volatile-only` and `checkpoint_error` rather than silently claiming durable
storage.

Legacy U/A/T (including I-header) logs are not restore input and are not
automatically migrated: their role loss and ambiguous line framing cannot be
repaired safely. Start a new checkpoint, or use gateway `"reset":true` for an
exact chat ID. Existing files remain visible to `/sessions` only as legacy
review text.

Reference host measurements on 2026-11-16 (Linux x86_64, gcc, stripped unless
noted; baseline `2fc9b9a`) are:

| Build / measurement | Bytes / sections / RSS | Relative result |
| --- | ---: | ---: |
| Baseline default full CLI | 162,040 bytes; text 148,549; data 2,664; BSS 167,960 | baseline |
| Checkpoint full CLI (`aaf0730`) | 162,040 bytes; text 150,066; data 2,816; BSS 167,944 | +0 file; +1,517 text; +152 data; -16 BSS; below 163,840 guard |
| Live/replay retention full CLI (`2d8a7ad`) | 162,040 bytes; text 150,799; data 2,800; BSS 167,944 | vs `aaf0730`: +0 file; +733 text; -16 data; BSS unchanged |
| Core-only nofs probe with retention (`2d8a7ad`) | 56,744 bytes unstripped; text 40,093; data 808; BSS 4,224 | +4,480 file; +5,102 text; +8 data; BSS unchanged; zero checkpoint symbols |
| Memory-codec full CLI | 162,040 bytes; text 152,713; data 2,800; BSS 167,944 | vs `2d8a7ad`: +0 file; +1,914 text; data/BSS unchanged; below 163,840 guard |
| Core-only nofs probe with memory codec | 65,464 bytes unstripped; text 47,111; data 952; BSS 4,224 | vs `2d8a7ad`: +8,720 file; +7,018 text; +144 data; BSS unchanged; no host file symbols |
| Near-cap valid restore | 1,006,563-byte snapshot; 256 messages | 10.3 ms REPL startup + restore; 18,132 KiB peak child RSS (checkpoint baseline measurement) |
| Near-cap restore + new bounded echo turn | input 1,006,563; output 991,187 bytes; 256 messages | 30 ms process elapsed; 12,568 KiB peak RSS; complete-turn eviction and atomic save |
| Default `tests/perf.sh` cold / 5-step loop peak RSS | 9,080 / 8,940 KiB | `2d8a7ad` baseline 9,136 / 8,968 KiB; 50 ms / 100 ms with the coarse 50 ms timer; guards pass |

Host CLI/harness, full-CLI agent/transport and the POSIX file backend use `-Os`
because their request/history/transport/file work is I/O-bound; the strict
core-only probe, config and other tool objects remain `-O2`. This keeps every
compiler in the compatibility matrix within the full-binary size guard.
The near-cap timing includes process/REPL startup and is not a pure parser
benchmark. Peak RSS includes libc, cJSON trees and transient copies; it is not
an MCU RAM budget. Snapshot writes are bounded by the same 1 MiB cap and happen
only after successful turns. Caller-owned memory snapshot buffers are
implemented and tested; concrete RTOS/HAL persistence, any additional
full-CLI persistence switch and actual MCU/RTOS Flash/RAM or hardware
validation remain open follow-up work. Memory
buffers alone are volatile and none of those unimplemented targets is claimed
here.

CI (`.github/workflows/ci.yml`) is split into per-area jobs, all gated
by the aggregate `ci` job (that exact name is the required status check
on main): `build` (gcc clean compile+link, size guard, artifact),
`clang` (full -Werror build), `test-core` / `test-tools` / `test-repl` /
`test-skills` / `test-schema` / `test-goal` (component-scoped smoke
groups, see `tests/smoke.sh <group>`), `plugins` (example .so with gcc +
clang -Werror), `tools` (component static libs compile independently
with clang -Werror), `no-filesystem` (strict-C11 core, memory snapshots and optional
file/shell backend cuts), `vision` (optional local-mock integration and
clang `-Werror`),
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

Sessions: `src/harness/sessions.c` owns state directories and legacy trace
listing; `src/core/checkpoint.c` owns validated full-turn memory
serialization/restoration, and `src/harness/checkpoints.c` adds bounded atomic
host-file I/O. `--save`, `-r`, REPL `/resume`/auto-save and gateway restore
all use the same snapshot contract. `motiris_session_append` is legacy
trace-only and must never be used as restore input.

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

- Smoke #4: default binary stays < 163840 bytes (`SMOKE_SIZE_MAX` may be
  raised only for instrumented builds such as ASAN/UBSAN).
- Smoke #6: full-turn CLI/REPL round-trip preserves multiline assistant and
  tool objects without line-prefix role injection; malformed/legacy/digest/
  pairing/message/record/file-cap failures are offline and rejected before
  the provider. Near-cap valid restore, failed-turn atomic preservation and
  PTY auto-save are included. Multi-turn item/byte retention, complete-turn
  eviction, restore-time limits and independent replay item/byte budgets run
  in `tests/history.py`; live overflow fails closed, and an over-budget replay
  active turn is rejected before transport. `make test-core-nofs` separately
  verifies byte-exact caller-owned memory round-trip and fail-closed ownership
  and digest checks under strict C11 without host file I/O.
- Smoke #9: gateway health/auth plus partial, byte-segmented, malformed
  and oversized requests. Headers are capped at 16 KiB and bodies at
  1 MiB; the receive allocation is capped at their sum plus one NUL byte.
  Gateway session regressions cover colliding punctuation, case, Unicode,
  120-byte IDs, full assistant restoration after restart and 64-session
  eviction, legacy/ownership/digest fail-closed behavior, and exact-ID reset.
- Smoke #11: shell drains both pipes concurrently, enforces per-stream/total
  output caps and a monotonic timeout, kills/reaps its process group, and
  survives repeated calls without fd/direct-child leaks (also under ASAN/UBSAN).
- Smoke #14: SSE delta aggregation (first-delta termination).
- Smoke #16: repl shows tool lines + token stats (streamed tool_calls
  + usage through the mock).
- Smoke #17: plugin tool hook fires (needs examples/hello_plugin.so).
- Smoke #20/#21: skill_patch / skill_write edit real skill files via
  the agent loop (mock model), matching the current schema strings —
  if you change a tool schema, update these mocks.
- Smoke #23: REPL `/resume` restores a full four-message echo turn and
  rejects legacy U/A/T rows rather than treating their contents as commands.
- Smoke #24-26: per-tool schema files (override / invalid json / plugin
  subdir layout) — if you change the merge logic or dir resolution,
  update these.
- Smoke #27: goal-check loop (`--goal-check` + DONE confirmation).
- Goal-check state is per-agent (goal from the agent's own messages,
  `confirm_asked` is a `motiris_run` local); no new global state — keep
  it that way when touch agent.c.
