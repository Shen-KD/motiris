# DEVELOPING motiris

Field notes for working on this repo: layout, build/test loop,
extension points, and the traps that have already burned time.

## Architecture

Single C11 binary; host builds use libcurl.so.4 and the platform C/POSIX
thread runtime (`-pthread`, folded into libc on current glibc). Components
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
make MEMORY_TOOLS=0   # optional persistent memory files -> no-I/O stubs
make PERSISTENCE=0    # optional host checkpoint files -> volatile sessions
make test-shell       # dual-stream, output cap, timeout and reaping tests
make test-browser-subagent # browser/subagent deadline and cleanup tests
make test-webtools-parser # malformed/truncated DDG response parser tests
make test-filetools   # malformed handlers, file bounds and atomic faults
make test-memory      # memory corruption, atomic fault and replacement tests
make test-nofs        # strict-C11 core probe + full-CLI backend-cut test
make test-persistence # full-CLI checkpoint-cut fail-closed/volatile tests
make VISION_TOOLS=1   # opt in to host-backed vision_analyze
make test-vision      # clean optional build + bounded local-provider tests
make TTS_TOOLS=1      # opt in to host-only REPL external TTS adapter
make test-tts         # clean optional build + fake-engine integration tests
```

### Platform matrix and resource bounds

| Target | Build/test evidence | Boundary |
| --- | --- | --- |
| Linux gcc / clang | `make`, `make test`, CI `-Werror` and ASAN/UBSAN | Full host CLI; libcurl and POSIX tools enabled |
| Host CLI without file/memory/shell backends | `make test-nofs` (`FILE_TOOLS=0 MEMORY_TOOLS=0 SHELL_TOOLS=0`) | Excludes `filetools.c`, `memorytools.c` and the shell subprocess implementation; file/memory tools return fixed bounded no-I/O errors and `shell` is unregistered. Other host tools/harness are unchanged |
| Host CLI without checkpoint files | `make test-persistence` (`PERSISTENCE=0`) | Excludes the stdio/`fsync`/atomic-`rename` backend; explicit file operations fail closed, REPL/gateway use volatile bounded history. Portable core memory snapshots remain; other host facilities are unchanged |
| Optional host vision | `make test-vision` (`VISION_TOOLS=1`) | One workspace-local image to a configured remote provider; separate POSIX file + libcurl backend, excluded by default and not local inference |
| Optional host TTS | `make test-tts` (`TTS_TOOLS=1`) | Final REPL text to one trusted external executable over a bounded pipe; fake-engine tests only, no audio device or MCU claim |
| Portable core-only probe | `make test-core-nofs` | Strict C11 `-Werror`; core loop/config/echo transport, bounded memory snapshot codec + cJSON, no POSIX feature macros, filesystem I/O or libcurl. A small test adapter supplies tool/schema registries; this is not the full CLI |
| MCU / RTOS | **待定 / 未验证** | Chip, RTOS, cross-toolchain, RAM/Flash and hardware test matrix have not been supplied; host results are not MCU support claims |

`search_files` keeps directory enumeration inside the optional POSIX file
backend. An explicitly named start path is canonicalized. With the default
`workspace_guard=false`, search reopens from `/`; with the guard enabled it
first checks workspace containment and reopens from the workspace directory
fd. Every component and recursive entry uses `openat(..., O_NOFOLLOW)`;
recursive symlinks (including loops) are skipped. This closes symlink
replacement between a pathname check and open without adding a core
dependency. Read/write/patch share the same per-Agent optional boundary while
retaining canonical paths, file type/size limits and atomic replacement.

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

### Fail-closed tool policy and bounded terminal replies

`--no-tools` is a per-Agent execution policy, not merely a built-in
registration filter. CLI, cron and Gateway disable the Agent before
`motiris_apply_config()`, which preserves that override instead of allowing
`tools:true` to re-enable it or initialize configured MCP children. Plugin and
MCP entry points return before directory scanning, `dlopen` or `fork`.
Core registration, provider schema assembly
and dispatch independently check the same flag, so a tool registered before
disabling is neither advertised nor executed. Registering while disabled is a
no-op; re-enabling does not discard the existing per-Agent registry. Skill
metadata indexing remains available, but no Skill/read tool is registered or
executed and bodies are not injected. Real subagents receive both `--no-tools`
and `--no-plugin`.

`motiris_cron_run_with_tools()` carries the CLI policy into each fresh job
Agent. The existing `motiris_cron_run()` keeps its signature and delegates
with config-driven tools enabled, so the public C API remains compatible.
The no-tools Skill index uses direct bounded regular-file reads rather than
`popen`: at most 4096 entries per directory, 128 Skills and 1 MiB per file.
Only frontmatter metadata reaches system context. It has no shell evaluation,
child process or full-body prompt injection; loading a body requires the
explicit built-in `read_file` tool and therefore is unavailable under
`--no-tools`.

Gateway and optional TTS use the const, Agent-owned `motiris_turn_start()`
accessor to scan only the latest external run in forward order. The pointer
is invalidated by message mutation/import or Agent destruction. Empty/null/
missing terminal content clears the current candidate instead of looping
through cJSON's circular `prev` link or reusing an older/intermediate reply.
A confirmation flag tracks the synthetic goal prompt across tool calls:
confirmation tool messages and `DONE` preserve the real answer, while real
`DONE` content answering an external goal-like user prompt remains visible/
spoken. No global turn state, queue, thread or approval state machine is
added; optional pending approval remains outside this change.

Reference host measurements on 2026-11-19 (Linux x86_64, gcc, stripped except
the strict probe; baseline `7bd95d6`) are:

| Build / measurement | Baseline | Policy/reply fixes | Relative result |
| --- | ---: | ---: | --- |
| Default full CLI | 186,624 bytes; text 175,698; data 3,144; BSS 169,040 | 190,720 bytes; text 177,356; data 3,128; BSS 169,040 | +4,096 file; +1,658 text; -16 data; BSS unchanged |
| Strict core-only nofs probe | 69,920 bytes unstripped; text 50,771; data 1,016; BSS 4,232 | 70,000 bytes unstripped; text 51,159; data 1,016; BSS 4,232 | +80 file; +388 text; data/BSS unchanged; libc only |
| Optional `TTS_TOOLS=1` full CLI | default above | 190,720 bytes; text 180,550; data 3,128; BSS 169,040 | +0 file and +3,194 text vs fixed default; 13 fake-engine cases pass |
| `PERSISTENCE=0` full CLI | 182,528 bytes; text 168,737; data 3,000; BSS 169,040 | 182,528 bytes; text 170,363; data 2,984; BSS 169,040 | +0 file; +1,626 text; -16 data; volatile/fail-closed tests pass |
| `FILE_TOOLS=0 SHELL_TOOLS=0 MEMORY_TOOLS=0` | not re-measured | 170,240 bytes; text 159,270; data 2,992; BSS 164,944 | optional backend-cut suite passes |
| `tests/perf.sh` cold / 5-step loop peak RSS | 9,324 / 9,248 KiB | 9,296 / 9,024 KiB | -28 / -224 KiB host variation; both 50 ms, guards pass |

Default and optional direct ELF dependencies remain only `libcurl.so.4` and
`libc.so.6`. The policy adds constant per-Agent/loop state; no approval
backend, dynamic queue or idle work is introduced. These are host application
bounds and relative measurements, not MCU Flash/RAM headroom. No MCU/RTOS
cross-toolchain, target or hardware validation was performed.

### Generation-time input, rendering and cancellation

A supported TTY keeps one incremental linenoise editor on the REPL main thread
for the whole session. Each ordinary turn runs `motiris_run` on one joinable
pthread. The worker exclusively owns the Agent until join; the main thread
exclusively owns terminal I/O, line editing/history, slash commands, cards and
post-run token/session/checkpoint access. Non-TTY and unsupported-terminal
paths retain the synchronous loop.

Worker callbacks deep-copy answer/final/reasoning/tool/diagnostic events into
a bounded 128-event/256-KiB queue (4-KiB text chunks, condition-variable
backpressure) and wake the main thread through a nonblocking, close-on-exec
self-pipe. The
main thread hides the prompt while writing ordered output, then restores the
current draft on a fresh line at the next key or turn boundary instead of
overwriting an unfinished answer. It temporarily restores `OPOST` while hidden
so newlines do not staircase. The aggregated `motiris_set_final_cb` path covers
echo, spawned-curl and `--no-stream`
answers without a direct core stdout write. The callback replaces only that
buffered print; streamed deltas still suppress the aggregate. Core verbose and
warning lines likewise use `motiris_set_diagnostic_cb` in the asynchronous TTY,
while other hosts retain their default stderr diagnostics.

Enter accepts at most 32 lines/64 KiB into a local FIFO. It never implicitly
cancels or edits an already-sent provider request. Queued ordinary input is
added to Agent history only at dispatch; local commands execute at idle
boundaries in the same order. Accepted lines enter linenoise history once,
but file saves wait until no worker is active because history saving changes
process `umask`. Unsubmitted drafts stay only in the editor. Raw Ctrl+C and a
lone Escape request the existing cooperative cancel flag without clearing the
draft/FIFO; after join, rollback and session finalization, the next line runs
automatically. Arbitrary tool calls still cannot be forcibly interrupted.
Lock-free C11 atomics give cancellation flags signal/cross-thread visibility,
and self-pipe writes retry `EINTR`. Escape/UTF-8/bracketed-paste parsers retain
partial state across bounded reads.

Approval is a condition-variable handoff: the worker waits while the main
thread runs the existing card as sole input reader, then resumes with the
same draft. EOF/HUP closes the event queue, denies/wakes a pending approval,
requests cancellation, joins the worker and restores signals/termios. No
`pthread_cancel`, concurrent linenoise access or concurrent Agent getters are
allowed. `tests/repl_generation_input.py` covers live editing, FIFO natural
continuation, local-command ordering, lone-Esc cancellation, exact user
history, draft preservation and final-screen answer visibility across buffered
and consecutive streamed deltas; existing cancel/cards/menu/render tests cover
Ctrl+C, approval arbitration and non-TTY compatibility.

### Provider routing, single rendering and bounded responses

Every nonempty configured provider list, including a singleton, selects the
entry's model/base_url/key in declaration order. The default provider is used
only when the list is empty. An omitted or unresolved entry `api_key_env`
resolves to NULL and is explicitly keyless: it never inherits the Agent or
`MOTIRIS_API_KEY` default credential. Authentication uses the existing fixed
512-byte stack buffer (22-byte `Authorization: Bearer ` prefix, at most 489
key bytes and a NUL), with no header allocation. The complete selected
provider chain is preflighted before any request; an overlong key returns
`authorization header too long` before network I/O and is never logged or
silently replaced with an unauthenticated request.

A per-attempt forwarding callback records whether
a streaming backend actually emitted nonempty text deltas. The agent suppresses
its aggregated final print only for that case; echo, spawned-curl and other
buffered fallback paths retain their one final print. Goal-check confirmation
uses no delta sink, so only a non-`DONE` aggregate is rendered. Once visible
deltas exist, a transport/parse/API failure ends that run rather than mixing a
partial answer with fallback output. The flag is run/attempt local, not global
agent or session state.

Provider assistant objects are canonicalized before entering per-Agent history:
only `role`, `content` and `tool_calls` participate in replay and snapshots.
Tool-call entries retain only `id`, `type` and `function`; function entries
retain only `name` and `arguments`, so nested provider extensions cannot leak
into the strict snapshot schema. Response-only metadata such as `refusal` and
`annotations` is discarded after
the bounded provider response has been parsed, so null/string/array metadata
shapes cannot make an otherwise successful turn fail snapshot validation. The
current response tree remains available for content/tool dispatch. Snapshot
import retains its strict unknown-field and duplicate-field rejection; this is
an ingestion boundary, not a relaxation of the persisted format. The temporary
response duplicate remains bounded by the existing response/receive limits and
adds no retained global or per-session metadata.

`MOTIRIS_RESPONSE_MAX_BYTES` configures `C` from 1 through 16 MiB (default and
hard maximum 16 MiB). Invalid values warn at Agent construction and use the
default, avoiding direct worker-thread diagnostics. Buffered
libcurl and spawned-curl bodies, each SSE line, aggregate content and each
tool-argument string retain at most `C` bytes plus a private NUL; total SSE
wire input is at most `4*C` to include JSON/framing, and at most 256 tool calls
are aggregated. Geometric allocation is below `2*(C+1)` per receive buffer.
Checked subtraction precedes every addition/allocation, a temporary pointer
preserves the original allocation on `realloc` failure, and limit/OOM paths
free partial state and return bounded `response limit exceeded` /
`response allocation failed` errors. Fallback may try the next entry only
before visible deltas. Tool-call indices must be finite integers in `0..255`;
missing, negative, fractional and huge indices return `invalid stream response`
before any numeric cast. Authenticated spawned-curl argv storage includes its
NULL terminator, and pipe/fork setup closes prior descriptors on failure.
The response-limit accounting itself adds no thread, runtime, global Agent
state or direct dependency; the independent TTY REPL worker is described above.

The cJSON tree for a line/response and final serialization add phase-wise
copies derived from those bounded inputs; exact libc/libcurl/cJSON node
overhead is implementation-dependent. These are per-call application bounds,
not a whole-process RSS guarantee or an MCU RAM budget. The host HTTP backends
remain outside the strict-C11 nofs probe; its echo path does not read this
setting. An RTOS/HAL network backend and target-specific smaller budgets still
need real platform validation.

`tests/provider.py` verifies 1/2/8-entry routing, independent and keyless
entries, omitted/missing key environments, mixed-chain credential isolation,
default-provider authentication, real fallback order, exactly zero/one
Authorization headers per request, and default/entry 489/490-byte boundaries.
A later overlong entry rejects the whole chain before an earlier healthy
provider can perform I/O, without leaking the key. `tests/transport.py`
covers buffered, SSE wire/line/content/tool arguments, spawned curl, an
exact-size body and echo. `tests/repl_cancel.py`
also asserts one rendered copy of a successful streamed follow-up.
`tests/render.py` covers streamed goal-check `DONE`, fail-after-delta fallback
suppression, and stream-enabled echo/spawned-curl REPL rendering. All run in
existing smoke groups and sanitizer CI jobs.

Reference host measurements on 2026-11-17 (Linux x86_64, gcc `-O2`/component
`-Os`, stripped; baseline `e570177`) are:

| Build / measurement | Bytes / sections / RSS | Relative result |
| --- | ---: | --- |
| Baseline default full CLI | 174,328 bytes; text 162,897; data 3,048; BSS 169,040 | baseline |
| Bounded provider full CLI | 174,328 bytes; text 164,576; data 3,040; BSS 169,040 | +0 file; +1,679 text; -8 data; BSS unchanged |
| Default `tests/perf.sh` cold / 5-step loop peak RSS | 9,276 / 9,168 KiB | baseline 9,140 / 9,072 KiB; 83 ms / 110 ms vs 50 ms / 50 ms; host variation, guards pass |
| 16 MiB buffered response rejected at cap | 28,232 KiB peak CLI RSS | GNU `time`; local provider, raw response slightly over cap; not an MCU budget |
| Strict core-only nofs probe | 69,840 bytes unstripped; text 49,715; data 952; BSS 4,232 | libc only; host receive-limit code compiled out |

Keyless/auth-preflight reference measurements on 2026-11-18 (Linux x86_64,
gcc `-O2`/component `-Os`, stripped except the core probe; baseline `235a4de`)
are:

| Build / measurement | Bytes / sections / RSS | Relative result |
| --- | ---: | --- |
| Default full CLI | 186,624 bytes; text 174,882; data 3,048; BSS 169,040 | +0 file; +128 text; data/BSS unchanged |
| Strict core-only nofs probe | 69,840 bytes unstripped; text 50,115; data 952; BSS 4,232 | +0 file; +400 text; data/BSS unchanged; libc only |
| Default `tests/perf.sh` cold / 5-step loop peak RSS | 9,180 / 8,972 KiB | baseline 9,088 / 9,072 KiB; both 50 ms before/after; host variation, guards pass |

Authentication still uses one fixed 512-byte stack buffer per provider attempt;
preflight adds no retained allocation, Agent state, thread or idle work. Default
direct ELF dependencies remain only `libcurl.so.4` and `libc.so.6`.
No MCU/RTOS cross-toolchain, target Flash/RAM or hardware validation is
available; none of these host numbers is an MCU support claim.

### Canonical session replay and NUL-safe gateway IDs

Assistant canonicalization reuses the existing bounded cJSON response duplicate
and retains no provider metadata or new Agent/global state. The gateway's
Content-Length NUL scan is allocation-free and O(n) over the existing 1 MiB
body cap. Default core dependencies and the snapshot format are unchanged;
`PERSISTENCE=0` still removes only the optional host checkpoint backend.

Reference host measurements on 2026-11-19 (Linux x86_64, gcc `-O2`/component
`-Os`, stripped except the core probe; baseline `7d39e66`) are:

| Build / measurement | Baseline | Session-isolation fix | Relative result |
| --- | --- | --- | --- |
| Default full CLI | 186,624 bytes; text 174,882; data 3,048; BSS 169,040 | 186,624 bytes; text 175,698; data 3,144; BSS 169,040 | +0 file; +816 text; +96 data; BSS unchanged |
| Strict core-only nofs probe | 69,840 bytes unstripped; text 50,115; data 952; BSS 4,232 | 69,920 bytes unstripped; text 50,771; data 1,016; BSS 4,232 | +80 file; +656 text; +64 data; BSS unchanged; libc only |
| Default `tests/perf.sh` cold / 5-step loop peak RSS | 9,232 / 9,276 KiB | 9,128 / 9,140 KiB | -104 / -136 KiB host variation; both 50 ms, guards pass |
| `PERSISTENCE=0` full CLI | not re-measured | 182,528 bytes; text 168,737; data 3,000; BSS 169,040 | vs fixed default: -4,096 file; -6,961 text; -144 data; BSS unchanged |

Default direct ELF dependencies remain only `libcurl.so.4` and `libc.so.6`.
The persistence-cut explicit-operation, PTY and volatile-gateway suite passes.
These are host binary/RSS measurements, not MCU Flash/RAM headroom. No
MCU/RTOS cross-toolchain, target or hardware validation was performed.

### Bounded MCP stdio I/O

`MOTIRIS_MCP_TIMEOUT_S` selects a 1..120-second request budget (default 10);
invalid values reject that MCP server before `fork`. Each initialize,
tools/list, tools/call and initialized-notification write gets one absolute
`CLOCK_MONOTONIC` deadline. Nonblocking `poll` covers partial writes, read
read-ahead, `EINTR` and early EOF without resetting that budget. Request and
response lines retain at most 1 MiB plus a private NUL. A blocked request write
and a silent response read have distinct bounded diagnostics. SIGPIPE is
ignored only around the single-threaded pipe write and restored afterward.

A fatal request failure closes both fds and marks the connection failed. The
small connection object remains alive for registered tool userdata, so later
calls free their argument tree and return the existing error immediately
without use-after-free or another wait. `mcp_request` takes ownership of
`params` on every path. The server calls `setsid`; cleanup signals the process
group before reaping its leader, allows one second of TERM grace, then sends
KILL and reaps the direct child. If the leader exits first, the group still
receives KILL so surviving descendants cannot escape cleanup. The terminal
post-KILL `waitpid` follows the existing shell/TTS reap contract: a task stuck
in an uninterruptible kernel wait is outside the userspace deadline guarantee.

Each configured server owns one fixed 4 KiB read-ahead array, a 192-byte
error buffer and fixed poll/deadline/process state. Dynamic request/response
lines are separately capped at 1 MiB; cJSON parsing/serialization creates
phase-wise copies derived from those caps. There is no request queue,
background thread, event library or idle polling. These allocations and POSIX
pipe/process operations remain in `src/harness/mcp.c`, outside the strict-C11
core-only probe; no new runtime or direct ELF dependency was added.

`tests/mcp.py` runs in smoke #13 and under the existing ASAN/UBSAN tools job.
It covers three initialize timeouts in one CLI, silent tools/list and
tools/call, an exactly-1-MiB request whose newline cannot fit a full pipe,
response overflow, deterministic EPIPE/SIGPIPE, TERM-ignoring direct children,
a TERM-ignoring descendant whose leader exits first, failed-connection retry,
and invalid configuration. The servers sample their motiris parent's live
`/proc` fd/RSS state at each repeated start and the test checks every direct
child/descendant is gone. The write test requires the explicit write-timeout
diagnostic, so a read-only timeout implementation cannot produce a false pass.

Reference host measurements on 2026-11-17 (Linux x86_64, gcc `-O2`/harness
`-Os`, stripped; baseline `00bf5d3`) are:

| Build / measurement | Baseline | MCP deadline branch | Relative result |
| --- | ---: | ---: | --- |
| Default full CLI | 178,432 bytes | 182,528 bytes | +4,096 bytes |
| ELF text / data / BSS | 166,496 / 3,048 / 169,040 | 169,962 / 3,056 / 169,040 | +3,466 / +8 / +0 bytes |
| Strict core-only nofs probe | 69,840 bytes; text 49,715; data 952; BSS 4,232 | 69,840 bytes; identical sections | unchanged; MCP harness absent |
| `tests/perf.sh` cold peak RSS | 9,184 KiB | 9,288 KiB | +104 KiB; both 50 ms, host variation |
| `tests/perf.sh` 5-step loop peak RSS | 9,372 KiB | 9,280 KiB | -92 KiB; 50 -> 100 ms, below 3 s guard |
| Three repeated initialize failures | fd/RSS sampled per server | fd 5 / 5 / 5; RSS 8,632 / 8,696 / 8,696 KiB | stable fds; +64 KiB then flat RSS |
| Full MCP failure-path integration | not present | 27,112 KiB max direct-child RSS | Python test driver high-water mark, including 1 MiB request/provider cases |

Default direct ELF dependencies remain only `libcurl.so.4` and `libc.so.6`.
These are host application bounds and measurements, not total libcurl/libc
RSS guarantees or MCU Flash/RAM headroom. No MCU/RTOS cross-toolchain or
hardware validation was performed.

### Bounded browser/subagent child processes

`src/tools/childproc.c` is a private host-tools runner shared by
`browser_fetch` and `subagent`; it is not public API and is absent from the
strict-C11 core probe. `MOTIRIS_BROWSER_TIMEOUT_S` defaults to 30 s and
`MOTIRIS_SUBAGENT_TIMEOUT_S` to 120 s. Both accept only 1..120 and reject
invalid values before `fork`. One absolute `CLOCK_MONOTONIC` deadline covers
pipe/fork setup, combined stdout/stderr draining and normal exit. Nonblocking
`poll`, partial reads and `EINTR` always recheck that deadline, so continuous
output cannot extend it.

The existing combined stdout/stderr contract is preserved. Browser retains at
most 65,536 bytes and subagent 32,768 bytes, each plus a private NUL. Excess
bytes are drained and discarded with `truncated=true`; the read scratch buffer
is 4 KiB and deadline/poll/wait state is fixed-size. cJSON construction and
Agent history add phase-wise copies derived from those caps, with additional
bounded JSON escaping; these are application-owned per-call bounds, not total
process RSS or MCU RAM guarantees. Results retain the original `text`/`output`
fields and add `exit_code`, `signal`, `timed_out`, `truncated` and bounded
`error` diagnostics. Tool input schemas and the plugin ABI are unchanged.

The child calls `setsid`. Timeout, setup, poll/read/wait or allocation failure
closes the parent pipe and signals the process group with TERM, allows one
second of grace, then sends KILL and reaps the direct child. If the leader
exits first, remaining group members are still killed, whether a descendant
holds the output pipe or redirected it to DEVNULL. A successful leader return
also kills remaining same-group background members. The post-KILL `waitpid`
has the same uninterruptible-kernel-wait caveat as shell/MCP. This is bounded
process-group cleanup, not a sandbox or general cancellation tree: descendants
that deliberately create another session are outside the process-group kill
guarantee. No thread, event library, queue, global Agent state or runtime
dependency was added.

`tests/browser_subagent.py` runs through the real provider/tool loop for both
tools. It covers normal and nonzero exits, missing executables, a 192 KiB
alternating stdout/stderr flood, silent and continuous-output children,
TERM-ignoring leaders, pipe-holding and DEVNULL descendants, repeated timeouts
followed by a healthy call, and invalid settings that must not fork. It checks
leader reaping, descendant termination, steady parent fd/RSS samples and no
remaining direct children. The test runs in smoke #13 and the ASAN/UBSAN tools
job; existing smoke #12/#13 normal-path coverage remains.

Reference host measurements on 2026-11-18 (Linux x86_64, gcc, stripped;
baseline `285502d`) are:

| Build / measurement | Baseline | Bounded child branch | Relative result |
| --- | ---: | ---: | --- |
| Default full CLI | 182,528 bytes | 186,624 bytes | +4,096 bytes |
| ELF text / data / BSS | 169,962 / 3,056 / 169,040 | 173,042 / 3,048 / 169,040 | +3,080 / -8 / +0 bytes |
| Strict core-only nofs probe | 69,840 bytes; text 49,715; data 952; BSS 4,232 | 69,840 bytes; identical sections | unchanged; host tools absent |
| `tests/perf.sh` cold peak RSS | 9,288 KiB | 9,320 KiB | +32 KiB host variation; both 50 ms |
| `tests/perf.sh` 5-step loop peak RSS | 9,256 KiB | 8,972 KiB | -284 KiB host variation; both 50 ms |
| Three browser timeouts, parent fd/RSS samples | not bounded | fd 4 / 4 / 4; RSS 12,044 / 12,044 / 12,044 KiB | stable; later healthy call, no direct children |
| Three subagent timeouts, parent fd/RSS samples | not bounded | fd 4 / 4 / 4; RSS 12,016 / 12,016 / 12,016 KiB | stable; later healthy call, no direct children |

Default direct ELF dependencies remain only `libcurl.so.4` and `libc.so.6`.
These are Linux host application bounds and measurements, not MCU Flash/RAM
headroom. No MCU/RTOS cross-toolchain or hardware validation was performed.

### Bounded Gateway connection I/O

`src/harness/gateway.c` keeps the sequential, single-threaded accept loop but
sets each accepted socket nonblocking. `MOTIRIS_GATEWAY_IO_TIMEOUT_S` selects
a 1..120-second budget (default 30); malformed or out-of-range values fail
before the listening socket is created. One absolute `CLOCK_MONOTONIC`
deadline covers the complete header+body receive, and a fresh one covers each
complete response header+body send. `poll` always waits only for the remaining
interval. Byte progress, partial writes, `EAGAIN` and `EINTR` recheck rather
than extend that interval, so half-open and continuously trickling clients
cannot hold the only connection handler indefinitely. Agent/provider execution
is outside these socket-I/O budgets and retains its own transport bounds.

Receive expiry returns HTTP `408` with a bounded `request timeout` error.
Response-write expiry emits `response write timed out` to stderr. Either path
closes only the current socket and resumes `accept`, so subsequent health and
chat requests remain available. Header/body write failures and short writes
are handled instead of discarded. No threads, event library, waiting queue or
new runtime dependency is introduced. The only added per-connection state is
fixed-size stack storage for deadlines, one `pollfd` and offsets; the
process-wide timeout setting is immutable after startup and contains no
Agent/session data. The existing receive allocation remains at most 16 KiB + 1 MiB + 1 byte,
and response strings retain their existing bounded producer/history limits.

Before cJSON decodes `/v1/chat`, the gateway scans the explicit Content-Length
body for raw NUL bytes and JSON `\u0000` escapes in strings, validates all four
hex digits of every Unicode escape, then uses `cJSON_ParseWithLength`. Malformed
escapes are rejected instead of allowing cJSON's invalid-hex-as-zero conversion
to synthesize NUL. This allocation-free O(n) scan rejects HTTP 400 before
reset, session lookup or snapshot I/O, preventing a C-string prefix from
aliasing another chat ID or truncating a message. Escape parity is honored, so
a double-escaped literal `\\u0000` remains a distinct valid ID. The existing
1 MiB body bound also bounds this scan; no ID-length copy, thread or retained
state is added. Escaped/raw-NUL and malformed-Unicode reset rejection, snapshot
byte preservation, restart isolation and literal-backslash acceptance run in
smoke #9.

Reference host measurements on 2026-11-17 (Linux x86_64, gcc `-O2`/harness
`-Os`, stripped; baseline `57d3122`) are:

| Build / measurement | Baseline | Gateway deadline branch | Relative result |
| --- | ---: | ---: | --- |
| Default full CLI | 174,328 bytes | 174,336 bytes | +8 bytes |
| ELF text / data / BSS | 164,576 / 3,040 / 169,040 | 165,888 / 3,048 / 169,040 | +1,312 / +8 / +0 bytes |
| Strict core-only nofs probe | 69,840 bytes unstripped | 69,840 bytes; text 49,715; data 952; BSS 4,232 | unchanged; libc only |
| `tests/perf.sh` cold peak RSS | 9,188 KiB | 9,204 KiB | +16 KiB; both 50 ms, normal host variation |
| `tests/perf.sh` 5-step loop peak RSS | 9,360 KiB | 9,280 KiB | -80 KiB; both 50 ms, normal host variation |
| Five receive expiries + health checks | 8,792 KiB; 4 fds | 8,796 KiB; 4 fds | no fd growth; +4 KiB host-noise RSS |

Default direct ELF dependencies remain only `libcurl.so.4` and `libc.so.6`.
The strict core probe does not compile Gateway and remains unchanged. The
optional host checkpoint cut uses the same bounded socket path; no new
core/optional switch or dependency was added. `tests/gateway_io.py` covers
half-open headers and bodies, continuous trickle input, a non-reading client
with a 12 MiB local-provider reply, repeated expiry/fd stability, subsequent
health checks and invalid configuration. It runs in smoke #9, including the
ASAN/UBSAN tools job. These are Linux host measurements, not MCU Flash/RAM
headroom; no MCU/RTOS cross-toolchain or hardware validation is available.

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
the REPL selects the current run's terminal assistant message, skips
confirmation tool rounds and goal-check's synthetic `DONE`, and passes the
real answer to `MOTIRIS_TTS_CMD` with `exec`, not a shell. Empty terminal
replies do not invoke an engine or reuse older/intermediate speech.
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

Default and optional direct ELF dependencies remain only `libcurl.so.4` and
`libc.so.6`. The TTS backend itself adds no direct dependency; the shared run
accessor and current core/header deltas are measured in the policy section
above. These are host binary/RSS measurements, not target Flash/RAM
headroom. `tests/tts.sh` covers 13 fake-engine cases: exact text, backend
failure, timeout/reaping, truncation, immediate and tool-mediated goal-check
filtering, real-versus-synthetic `DONE`, empty/current-turn selection, missing
engine, invalid timeout and runtime-disabled behavior. No real speech engine, audio
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
| Bounded-shell default full CLI | 162,040 bytes; text 148,549; data 2,664; BSS 167,960 | Historical artifact/section measurements before the size-gate removal |
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

### Safe DDG result-row parsing

`web_search` validates the href opening/closing quotes, tag end and closing
`</a>` before computing lengths or copying a result. Malformed and truncated
candidates are skipped without consuming a later valid row. Candidate
boundaries require an actual `<a ... rel="nofollow"` row start, so literal
marker text inside a valid title is retained. Candidate discovery and all row
delimiter searches use bounded candidate spans rather than repeated
whole-suffix searches, avoiding quadratic malformed-input behavior. Title
extraction starts after
the validated `>` rather than at an offset derived from a truncated link. The parser still returns at most 10 results. Its fixed
`link[512]` and `title_text[256]` arrays occupy 768 bytes per active parser
invocation; each result retains at most 511 URL bytes and 200 title bytes plus
bounded cJSON node/string-escaping overhead. The input remains the shared
65,537-byte receive allocation, and no parser state survives the call.

`make test-webtools-parser` includes the production C file in a direct test
harness and uses exact-size heap responses. It covers the former missing-quote
511-byte OOB read, malformed rows and recovery at the following valid row,
every truncation prefix both alone and before a valid sentinel row, 1,000
consecutive incomplete candidates before a valid row, literal marker text in a
title, 1,000 deterministic random malformed inputs, 511/512/513/4096-byte
links, long titles, normal rows and the 10-result cap. Smoke #11 runs it; the CI sanitizer job compiles the same harness with
ASAN/UBSAN. No runtime, public tool schema/ABI, agent state or default core
dependency changed, and the strict-C11 core-only probe still excludes web tools.

Reference host measurements on 2026-11-18 (Linux x86_64, gcc, stripped;
baseline `2b47713`) are:

| Build / measurement | Baseline | Parser fix | Relative result |
| --- | ---: | ---: | ---: |
| Default full CLI | 174,336 bytes; text 165,968; data 3,048; BSS 169,040 | 178,432 bytes; text 166,496; data 3,048; BSS 169,040 | +4,096 file; +528 text; data/BSS unchanged |
| Strict core-only nofs probe | 69,840 bytes unstripped; text 49,715; data 952; BSS 4,232 | 69,840 bytes; identical sections | unchanged; libc only, web tools absent |
| `tests/webtools.py` max child RSS, including 1 MiB response | 21,920 KiB | 22,152 KiB | +232 KiB host run variation; full agent/provider/libcurl rounds |
| `tests/perf.sh` cold / 5-step loop peak RSS | 9,164 / 9,156 KiB | 9,380 / 9,132 KiB | +216 / -24 KiB host run variation; guards pass |

Default direct ELF dependencies remain only `libcurl.so.4` and `libc.so.6`.
These are host binary/RSS measurements, not MCU Flash/RAM headroom. No
MCU/RTOS cross-toolchain or hardware validation was performed.

### Validated, bounded and atomic file tools

`read_file`, `write_file` and `patch` validate every required JSON string
before path resolution or file mutation. Missing, null, numeric and array
values return deterministic bounded errors; empty `content` and `new` strings
are valid, while `old` must be non-empty. The `patch` schema now declares `new`
as required. `memory_set` likewise rejects a missing or wrongly typed `value`
before loading or detaching persistent state, while preserving explicit empty
string values.

A shared regular-file reader uses `O_NONBLOCK`, `fstat`, exact-size allocation
and complete-read checks. Input and patched output are capped at 1 MiB, with at
most 1 MiB + 1 byte for each owned read/patch buffer. Patch temporarily owns
both buffers before the old one is freed; cJSON argument trees and result
serialization add phase-wise implementation-dependent overhead. `write_file`
content is independently capped at 1 MiB. FIFOs and other non-regular files
are rejected without a blocking read.

Writes preserve basic `0777` permission bits on a short same-directory
`mkstemp` file, handle partial writes and `EINTR`, check `fsync`/`close`, then
atomically `rename`. Allocation, open, write, sync, close and rename failures
return an error, attempt temporary-file cleanup and preserve the old bytes.
Ownership, ACLs, xattrs and directory-fsync crash durability are not added.
No runtime,
thread, global Agent state or default core dependency was added; these host
tools remain outside the strict-C11 core-only probe and retain their existing
`FILE_TOOLS=0` / `MEMORY_TOOLS=0` no-I/O cuts.

`tests/filetools_unit.c` includes the production translation unit and wraps
open/read/allocation/temp-create/write/fsync/close/rename for deterministic
fault injection. It covers malformed arguments, explicit empty strings,
1 MiB boundaries, oversized input, FIFO rejection, short writes/reads,
permission preservation and byte-for-byte failure preservation. It runs in
smoke #11 and both ASAN/UBSAN and leak-enabled CI steps. `tests/memory_unit.c`
also covers missing/wrongly typed values and explicit empty strings.

Reference host measurements on 2026-11-18 (Linux x86_64, gcc, stripped;
baseline `e08e2cc`) are:

| Build / measurement | Baseline | Fixed branch | Relative result |
| --- | ---: | ---: | --- |
| Default full CLI | 186,624 bytes; text 173,042; data 3,048; BSS 169,040 | 186,624 bytes; text 174,754; data 3,048; BSS 169,040 | +0 file; +1,712 text; data/BSS unchanged |
| Strict core-only nofs probe | 69,840 bytes unstripped; text 49,715; data 952; BSS 4,232 | identical file and sections | unchanged; host file tools absent |
| `tests/perf.sh` cold peak RSS | 9,304 KiB | 9,156 KiB | -148 KiB host variation; both 50 ms |
| `tests/perf.sh` 5-step loop peak RSS | 8,864 KiB | 9,156 KiB | +292 KiB host variation; both 50 ms and below guards |
| Direct filetools unit maximum RSS | test not present | 6,040 KiB | New boundary/fault suite; GNU `time`, not an MCU budget |
| Memory maximum-value integration RSS | 13,100 KiB existing reference | 13,072 KiB | -28 KiB host run variation; valid-path coverage unchanged |

Default direct ELF dependencies remain only `libcurl.so.4` and `libc.so.6`.
These are host application bounds and measurements, not MCU Flash/RAM
headroom. No MCU/RTOS cross-toolchain or hardware validation was performed.

### Bounded, failure-safe persistent memory files

`memory_set` / `memory_get` use the optional host
`$HOME/.local/share/motiris/memory.json` backend. `MEMORY_TOOLS=0` replaces
`src/tools/memorytools.c` with `memorytools_nofs.c`: both tool schemas remain
registered, but calls return a fixed bounded error and never create/read a
file. `make test-nofs` verifies this cut together with the file/shell cuts;
the strict-C11 core-only probe never included these host tools and is unchanged.

Only `ENOENT` creates an empty in-memory object. Empty, malformed, non-object,
oversized, non-regular and short/incompletely read files fail both get and set
with bounded errors. Set never treats invalid existing data as empty and never
opens the original for truncating writes. Keys are limited to 256 bytes, values
to 256 KiB, paths to 4096 bytes including NUL, and input/serialized files to
1 MiB. A successful update serializes first, writes a same-directory
`mkstemp` mode-0600 file completely, checks `fsync` and `close`, then uses
atomic `rename`. Every failure before rename removes the temporary file and
preserves the original bytes. Detached cJSON replacements use `cJSON_Delete`.
There is no multi-process read/modify/write lock or idle/background I/O.

The read allocation is `file_size + 2`, at most 1 MiB + 2 bytes. Parsing adds
a cJSON tree derived from that bounded input; listing temporarily duplicates
it, while set owns a separate serialized buffer capped at 1 MiB plus NUL.
Exact cJSON/libc node overhead is implementation-dependent, so these are
phase-wise application bounds rather than total-process RSS guarantees.
Tool arguments and outer Agent history have their own existing bounds.

Reference artifact measurements on 2026-11-16 and runtime RAM measurements
on 2026-11-17 (Linux x86_64, gcc, stripped; baseline `4f36c9d`) are:

| Build / measurement | Bytes / sections / RSS | Relative result |
| --- | ---: | --- |
| Baseline default full CLI | 162,040 bytes; text 151,409; data 2,800; BSS 167,944 | baseline |
| Failure-safe memory full CLI | 162,040 bytes; text 153,067; data 2,808; BSS 167,944 | Informational artifact/section sizes; no binary-size CI gate |
| Ubuntu 22.04 GCC 11 / Clang full CLI | 162,048 / 163,096 bytes | Informational sizes after dropping C-unneeded unwind tables |
| `FILE_TOOLS=0 MEMORY_TOOLS=0 SHELL_TOOLS=0` full CLI | 145,656 bytes; text 136,347; data 2,648; BSS 163,848 | Combined optional-backend cut; memory host object absent and no-I/O stub tests pass |
| Strict core-only nofs probe | 65,464 bytes unstripped; text 47,111; data 952; BSS 4,224 | Unchanged from baseline; libc only, no host memory/file backends |
| Maximum 256 KiB value through local provider | 13,100 KiB peak process RSS | GNU `time`; includes two Agent/provider rounds, cJSON and libcurl; not an MCU RAM budget |
| Default `tests/perf.sh` cold / 5-step loop peak RSS | 9,196 / 9,200 KiB | Baseline 9,088 / 9,072 KiB; both 50 ms and below guards; normal host variation |

`tests/memory_unit.c` includes the production translation unit with syscall
fault injection for short reads and write/`fsync`/close/rename failures. It
also covers invalid/empty/non-object/oversized input, exact key/value limits,
mode 0600, byte preservation and repeated replacements. `tests/memory.py`
drives separate real CLI/provider processes for round-trip, corruption,
nonblocking FIFO rejection, write-failure and maximum-value coverage. GNU
`time` records the real CLI peak RSS; compatibility and Docker test images
install the test-only `time` package. `make test-memory` runs both; CI runs
them under ASAN/UBSAN with leak
detection enabled. The default direct
ELF dependencies remain only `libcurl.so.4` and `libc.so.6`. No MCU/RTOS
cross-toolchain, target Flash/RAM or hardware validation is available.

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

Provider assistant history is canonicalized to `role`, `content` and
`tool_calls` at ingestion. Other provider response metadata is not replayed or
persisted; imported snapshots still fail closed on unknown fields.

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

The full CLI defaults to `PERSISTENCE=1`. `PERSISTENCE=0` selects
`src/harness/checkpoints_nofs.c` instead of the host backend and defines
`MOTIRIS_NO_PERSISTENCE` for the REPL/gateway call sites. The stubs preserve
the host-file API but never inspect or modify a supplied path: explicit
`--save`, `-r` and `/resume` return `snapshot persistence unavailable`.
The REPL does not allocate an automatic checkpoint ID; the gateway neither
loads/creates snapshots nor unlinks one during reset. Its in-process session
list remains bounded at 64 chats and replies use `checkpoint=volatile-only`.
After restart, history starts empty instead of pretending to be durable.
This switch does not remove unrelated host tools, transports or gateway
POSIX dependencies, and is not an MCU port. The core archive and strict
memory-codec probe are unchanged. `tests/persistence.py` covers explicit
operations, TTY auto-save suppression, volatile gateway continuation,
restart/reset behavior and no file creation/deletion; `tests/persistence.sh`
also checks that the checkpoint object and atomic temporary-file marker are
absent and the selected stub has no `fsync`/`rename` imports. The independent
memory backend may still use atomic replacement.

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
| Checkpoint full CLI (`aaf0730`) | 162,040 bytes; text 150,066; data 2,816; BSS 167,944 | Historical artifact/section measurements before the size-gate removal |
| Live/replay retention full CLI (`2d8a7ad`) | 162,040 bytes; text 150,799; data 2,800; BSS 167,944 | vs `aaf0730`: +0 file; +733 text; -16 data; BSS unchanged |
| Core-only nofs probe with retention (`2d8a7ad`) | 56,744 bytes unstripped; text 40,093; data 808; BSS 4,224 | +4,480 file; +5,102 text; +8 data; BSS unchanged; zero checkpoint symbols |
| Memory-codec full CLI (gcc 15) | 162,040 bytes; text 151,409; data 2,800; BSS 167,944 | Historical artifact/section measurements before the size-gate removal |
| Memory-codec full CLI (Ubuntu 22.04 gcc 11) | 162,048 bytes; text 152,806; data 2,792; BSS 167,944 | Historical container build + full 27-check smoke record |
| Core-only nofs probe with memory codec | 65,464 bytes unstripped; text 47,111; data 952; BSS 4,224 | vs `2d8a7ad`: +8,720 file; +7,018 text; +144 data; BSS unchanged; no host file symbols |
| `PERSISTENCE=0` full CLI | 157,944 bytes; text 143,870; data 2,632; BSS 167,944 | vs `c78f59d`: -4,096 file; -7,539 text; -168 data; BSS unchanged; checkpoint backend/marker absent |
| Persistence-switch default / cut perf cold / 5-step loop peak RSS | default 9,116 / 9,072 KiB; cut 9,100 / 9,084 KiB | baseline 9,088 / 9,088 KiB; 50 ms / 50 ms; normal host variation; guards pass |
| Near-cap valid restore | 1,006,563-byte snapshot; 256 messages | 10.3 ms REPL startup + restore; 18,132 KiB peak child RSS (checkpoint baseline measurement) |
| Near-cap restore + new bounded echo turn | input 1,006,563; output 991,187 bytes; 256 messages | 30 ms process elapsed; 12,568 KiB peak RSS; complete-turn eviction and atomic save |
| Default `tests/perf.sh` cold / 5-step loop peak RSS | 9,064 / 9,024 KiB | `2d8a7ad` baseline 9,136 / 8,968 KiB; 50 ms / 50 ms; guards pass |

Host CLI/harness, full-CLI agent/transport/checkpoint and the POSIX file
backend use `-Os` because their request/history/snapshot/file work is
I/O-bound or size-sensitive; the strict core-only probe, config and other tool
objects remain `-O2`. Keeping the full-CLI snapshot codec at `-Os` preserves
the same size policy it had in the harness. Its full-CLI object also omits
C-unneeded asynchronous/unwind tables; the strict probe retains normal unwind
metadata. These are artifact-size choices only: no binary-size CI gate is
enforced, and runtime RSS remains the optimization signal.
The near-cap timing includes process/REPL startup and is not a pure parser
benchmark. Peak RSS includes libc, cJSON trees and transient copies; it is not
an MCU RAM budget. Snapshot writes are bounded by the same 1 MiB cap and happen
only after successful turns. Caller-owned memory snapshot buffers are
implemented and tested; the buffers alone are volatile. The full-CLI
`PERSISTENCE=0` switch is now implemented and tested, while concrete RTOS/HAL
persistence plus actual MCU/RTOS cross-build, RAM/Flash and hardware validation
remain open follow-up work; none of those unimplemented targets is claimed.
Default and cut direct ELF dependencies remain only `libcurl.so.4` and
`libc.so.6`; the strict core-only probe stays at 65,464 bytes with unchanged
sections.

CI (`.github/workflows/ci.yml`) is split into per-area jobs, all gated
by the aggregate `ci` job (that exact name is the required status check
on main): `build` (gcc clean compile+link and artifact),
`clang` (full -Werror build), `test-core` / `test-tools` / `test-repl` /
`test-skills` / `test-schema` / `test-goal` (component-scoped smoke
groups, see `tests/smoke.sh <group>`), `plugins` (example .so with gcc +
clang -Werror), `tools` (component static libs compile independently
with clang -Werror), `no-filesystem` (strict-C11 core, memory snapshots and optional
file/memory/shell backend cuts), `vision` (optional local-mock integration and
clang `-Werror`),
`tts` (optional fake-engine integration and clang `-Werror`),
and `sanitizers` (ASAN/UBSAN tools smoke plus leak-enabled memory fault,
config-update and REPL interruption tests). The `no-filesystem` job also
runs `make test-persistence` and a clang `-Werror` build/test for the full-CLI
host-checkpoint cut.
`perf` and `compat-matrix` are informational
(ubuntu 22.04 gcc-11 / 24.04 gcc-13 / 26.04 gcc-15 / centos-stream9
container / clang).

## Release & install

- Tag `vX.Y.Z` -> `.github/workflows/release.yml` builds, runs smoke,
  packages the main and optional context-compaction tarballs, their separate
  sha256 files (each names its archive), and `motiris-info.txt`.
- `install.sh` verifies new archives before extraction into empty directories,
  rejects unexpected members, and retains a strict legacy binary-hash fallback.
  It bootstraps defaults without overwriting user files. Interactive users can
  select context compaction; automation uses `--plugins=context-compaction` or
  `--plugins=none`. Session/read/write/MCP/approval are built in.
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
6. After every merge to `main`, rebuild from a clean tree and overwrite the
   dev machine's live copy: `make clean && make install PREFIX="$HOME"`, then
   verify `"$HOME/bin/motiris" --version`.

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
const MotirisPluginApi*)`. API v2 offers `register_tool` / `unregister_tool` /
`add_tool_hook` plus additive `set_compactor`. Tool observers are multi-slot;
the compaction callback is synchronous, deterministic and returns success or
failure before any old context is removed. `plugins/context_compaction/`
implements bounded extractive summaries without recursive model calls. Its
`SKILL.md` is installed with the plugin and updated with behavior changes.

Optional vision: `VISION_TOOLS=1` registers `vision_analyze` through the same
per-agent registry in CLI, REPL, gateway and cron builds.
`motiris_base_url` / `motiris_transport_name` expose the selected default
provider without making the agent struct public; `motiris_api_key` remains
limited to the optional vision build, while the model card uses only the boolean
`motiris_api_key_configured` query. Vision-specific `MOTIRIS_VISION_*` overrides
are read per call; no global session state is added.

The REPL `/model` card edits the default provider through public Agent APIs.
`think_mode=provider` omits the field, `on`/`off` send a JSON boolean, and
`max_tokens=0` omits the numeric field. `motiris_fetch_models` is explicit and
synchronous: it derives `/models`, shares default credential/auth bounds with
generation, and returns a caller-owned cJSON array. Its bounded libcurl-only GET
uses no redirect following, a 15-second timeout, a 1 MiB response cap, at most
128 IDs and 512 bytes per ID. The card owns and frees that array, sanitizes
terminal rendering and invalidates it on endpoint/key changes or refresh failure.
Saving uses the existing atomic config editor plus `motiris_set_env`, which
preserves unrelated env lines and replaces the mode-0600 file atomically without
printing secrets. Plaintext `api_key` remains an invalid config.json key.
Enter expands fetched model IDs into a bounded scrolling option window; Enter
applies and q/Escape cancels without mutating the selection, while `e` retains
manual editing. Dirty state belongs to `ReplState`, so declining save and
reopening still offers a later save; unchanged values do not prompt. Closing
with dirty values prompts for save/session-only/return, failed saves keep the
card open, and distinct EOF/interrupt keycodes force a session-only close
without a confirmation loop. `/model`, `/config`, approval and text
editor loops redraw only on actual input/state changes, never on the 100ms
raw-read timeout. `src/harness/` is the host runtime adapter layer (REPL,
gateway, cron, sessions, plugins and MCP), not the test directory.

Agent callbacks and run boundaries:

- `motiris_turn_start` — const Agent-owned pointer to the latest external
  run's first message; synthetic goal-check messages do not move it. Use only
  until the next message mutation/import or Agent destruction.
- `motiris_cron_run_with_tools` — cron CLI policy variant; the original
  `motiris_cron_run` remains config-driven and source-compatible.
- `motiris_set_stream_cb` — answer-delta sink (single slot; repl/gateway owns it).
- `motiris_set_reasoning_cb` — provider-only reasoning side channel; never
  canonicalized or persisted. `motiris_set_print_answer` lets Gateway suppress
  duplicate core stdout without changing REPL/CLI output.
- `motiris_set_final_cb` — receives an aggregate only when no answer deltas
  arrived; when set, it replaces that answer's core stdout print so a UI host
  can serialize buffered/echo/fallback output with its own renderer.
- `motiris_set_tool_hook` / `motiris_add_tool_hook` — called after each
  tool executes with (name, args, result_json, ud); result may contain
  `"error"` — the repl renders that as a red ✗ line.
- `motiris_tokens_in/out` — cumulative usage across all turns of an
  agent (parsed from `usage` in every response, streamed or not).

MCP: `"mcp_servers": [{"name","cmd","args":[],"env":{}}]` in
config.json spawns a stdio JSON-RPC server; each remote tool registers
as `<server>:<tool>`. `env` overrides are applied in the child process
before exec (not yet implemented for arbitrary vars — see mcp.c).

Sessions: `src/harness/session_store.c` owns the automatic UUID index and
canonical `session-<uuid>.json` traces under `MOTIRIS_HOME/session/`. REPL and
Gateway use atomic latest/selected restore and reset; reasoning is excluded.
`src/harness/sessions.c` owns legacy trace listing; `src/core/checkpoint.c` owns validated full-turn memory
serialization/restoration, and `src/harness/checkpoints.c` adds bounded atomic
host-file I/O. `--save`, `-r`, REPL `/resume`/auto-save and gateway restore
all use the same snapshot contract. `PERSISTENCE=0` replaces the host backend
with `checkpoints_nofs.c` fixed-error stubs and suppresses implicit REPL/gateway
file operations while preserving in-memory sessions and the portable core
codec. `motiris_session_append` is legacy trace-only and must never be used
as restore input.

Skill files: discovery merges user < workspace < explicit directories and
injects only metadata. `read_file` may read an exact canonical indexed user
Skill outside a guarded workspace, but never a new arbitrary external guarded
path. Unguarded file tools may access any OS-permitted path. `skill_patch` /
`skill_write` edit
indexed files by their stored canonical paths, never user-selected paths.

Per-tool schemas: `src/tools/toolscan.c` indexes
`<plugin_dir>/<name>/<name>.json` and `<tools_dir>/<name>/<name>.json`
(plugin dir = MOTIRIS_PLUGIN_DIR or ~/.motiris/plugins; tools dir =
MOTIRIS_TOOLS_DIR or ~/.motiris/tools). `motiris_register_tool` swaps
in the external schema when one exists. `motiris_plugin_dir_default()`
lives in plugin.c and is shared with toolscan.

## Tests that must not regress

- Smoke #3: `--no-tools` wins over `tools:true` for ordinary CLI and cron;
  built-in/Skill/Plugin/MCP schemas and execution stay disabled, configured
  MCP children and Plugin init do not occur, text-only Skill injection cannot
  invoke a shell, and a real subagent starts no configured backends. The
  strict-C11 probe also registers before disabling and proves direct-C
  dispatch and per-Agent policy isolation.
- Smoke #4: run one real cold-start echo turn and record peak RSS with GNU
  `time`. There is no binary-size gate; the measurement is a runtime-RAM
  optimization signal, not an artifact limit. Test images install `time`.
- Smoke #6: full-turn CLI/REPL round-trip preserves multiline assistant and
  tool objects without line-prefix role injection; malformed/legacy/digest/
  pairing/message/record/file-cap failures are offline and rejected before
  the provider. Near-cap valid restore, failed-turn atomic preservation and
  PTY auto-save are included. Multi-turn item/byte retention, complete-turn
  eviction, restore-time limits and independent replay item/byte budgets run
  in `tests/history.py`; live overflow fails closed, and an over-budget replay
  active turn is rejected before transport. Provider `refusal:null/string` and
  `annotations:[]/array` fixtures plus tool-call `index` and nested function
  extensions prove metadata is stripped at ingestion, while re-signed top-level
  and nested unknown-field snapshots remain rejected. `make test-core-nofs` separately
  verifies byte-exact caller-owned memory round-trip, fail-closed ownership
  and digest checks, and cancellation after a tool result under strict C11
  without host file I/O. The cancelled assistant/tool messages are removed,
  the active user remains, and the same agent then completes a retry.
  `make test-persistence`
  verifies the full-CLI `PERSISTENCE=0` cut: explicit operations fail closed,
  TTY auto-save is absent, gateway RAM history works but restarts are empty,
  reset does not unlink files. The checkpoint object/marker is absent and the
selected stub has no `fsync`/`rename` imports; the unrelated memory backend
may still use its own atomic replacement.
- Smoke #7: exported env precedence plus `set config` typed scalar updates.
  `tests/model_card_core.py` additionally compiles a strict C probe for
  think/max-token request serialization and reload, explicit-only `/models`
  discovery, endpoint/auth/error bounds, cancellation, and atomic mode-0600
  secret env updates without value logging. The offline integration checks JSON escaping, provider/MCP/extension field
  preservation, mode 0600, HOME fallback, concurrent readers during atomic
  rename, invalid/malformed/non-object/oversized input preservation, FIFO
  rejection without blocking, and cleanup after an actual `RLIMIT_FSIZE`
  temporary-write failure. No provider or plaintext `api_key` is involved.
- Smoke #9: gateway health/auth plus partial, byte-segmented, malformed
  and oversized requests. Headers are capped at 16 KiB and bodies at
  1 MiB; the receive allocation is capped at their sum plus one NUL byte.
  Half-open/trickling receives and a non-reading response client expire within
  the configured monotonic deadline, later health checks succeed, repeated
  timeouts leave fd counts unchanged, and invalid timeout settings fail closed.
  Gateway session regressions cover colliding punctuation, case, Unicode,
  120-byte IDs, full assistant restoration after restart and 64-session
  eviction, legacy/ownership/digest fail-closed behavior, and exact-ID reset.
  Raw/escaped embedded-NUL and malformed-Unicode IDs fail with HTTP 400 before
  reset/provider I/O without changing the aliased snapshot; restart replay
  excludes rejected messages and a double-escaped literal `\\u0000` ID remains valid.
  Empty/null/missing terminal replies, intermediate tool-call text, external
  goal-like prompts and tool-mediated confirmation return bounded current-run
  replies, followed by healthy requests; configured MCP remains disabled.
- Smoke #11: shell drains both pipes concurrently, enforces per-stream/total
  output caps and a monotonic timeout, kills/reaps its process group, and
  survives repeated calls without fd/direct-child leaks (also under ASAN/UBSAN).
  `make test-filetools` proves malformed-argument rejection, bounded regular
  file reads/patches, atomic failure cleanup and byte preservation.
  `make test-memory` additionally proves invalid-file preservation, bounded
  input/storage, atomic failure cleanup and leak-free repeated replacement;
  the dedicated sanitizer CI step enables LSan for both memory test layers.
- Smoke #14: SSE delta aggregation (first-delta termination).
- Smoke #15: piped REPL commands plus real-PTY live slash menus. Typing `/`
  displays all commands in aligned columns without Tab, `/s` and `/se` filter
  to the expected subsets, up/down move a wrapping reverse-video selection and
  Enter accepts it. No-match rows leave arrows on history navigation.
  `tests/repl_cards.py` drives the `/model` raw-terminal editor through model,
  think/token/reasoning, endpoint and hidden-key edits, proves zero I/O before
  Refresh, checks one authenticated GET and exact discovered-ID selection, then
  verifies config/env persistence, restart reload and sanitized 401 output.
  Inline fetched-model expansion covers navigation, exact selection, cancel
  without mutation and manual editing; close prompts cover save, session-only,
  return and in-card failure retry. Idle assertions prove `/model`, `/config`,
  approval and text editing emit no timeout redraws. Close/reopen and
  deterministic env-success/config-failure retries retain the pending credential;
  malicious OSC payloads cover summary, banner, raw editor, verbose diagnostics
  and terminal-safe rendering without changing stored IDs.
  `/hotkey` prints interactive key bindings, Ctrl-U clears old rows, `/quit`
  fails locally without a model turn, and `/exit` remains the single exit command. A slow-SSE PTY case also sends
  Ctrl+C during generation, verifies the REPL survives and returns its prompt,
  then appends an instruction and checks that the next request has both user
  messages but no partial assistant content. The generation-input PTY case
  additionally types/edits during SSE, queues multiple turns plus a local
  command, verifies FIFO requests and natural continuation, interrupts with
  lone Esc, runs the next queued turn, and submits the preserved draft without
  persisting partial assistant text.
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
- Smoke #27: goal-check loop (`--goal-check` + DONE confirmation). Gateway
  and optional TTS additionally cover confirmation tool calls, real versus
  synthetic `DONE`, and empty terminal messages without stale replies.
- Goal-check state is per-agent (goal from the agent's own messages,
  `confirm_asked` is a `motiris_run` local); no new global state — keep
  it that way when touch agent.c.
