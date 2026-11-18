# Changelog

All notable changes to motiris. Format keeps it terse: version, date,
breaking notes, then the highlights. Releases are tagged `vX.Y.Z` and
published by `.github/workflows/release.yml`.

## [Unreleased]

### Added

- Configurable provider receive bounds (`MOTIRIS_RESPONSE_MAX_BYTES`, default
  and hard maximum 16 MiB) cover buffered libcurl/spawned-curl bodies, SSE
  wire/line/content/tool-argument growth and streamed tool-call count.
  Overflow/allocation failures clean up partial state and return bounded
  errors; offline tests cover exact boundaries, malformed oversize responses,
  streaming fallback and the unchanged echo backend.
- `motiris set config KEY VALUE` edits whitelisted scalar JSON settings
  without agent/provider startup. Strings are escaped, booleans and
  `max_steps` retain their JSON types, and unrelated/provider/MCP/extension
  fields survive. Missing configs are created; valid updates use complete
  mode-0600 temporary writes, `fsync`/close checks and atomic rename. Invalid
  keys/values, malformed/empty/non-object/oversized/non-regular files and
  write failures preserve the original without logging the value. Offline
  tests cover escaping, concurrent readers, HOME fallback, FIFO rejection and
  a real `RLIMIT_FSIZE` write failure.
- REPL Ctrl+C now cooperatively cancels an in-flight generation without
  exiting or failing over to another provider. The active user instruction is
  retained, partial assistant/tool messages and token counts from that run are
  rolled back, and the next instruction continues the same agent. The public
  cancel callback and `MOTIRIS_RUN_CANCELLED` result cover libcurl, spawned
  curl and strict-C11 echo transports; real-PTY and nofs tests cover stream
  interruption, history rollback and a successful follow-up.
- REPL slash commands show a live description list as soon as `/` is typed
  and filter it on every prefix edit, while Tab completion and inline hints
  remain available. Up/down highlight and cyclically select rows, Enter runs
  the selection, and `/hotkey` lists interactive key bindings. `/exit` is the
  single exit command; unknown commands fail locally instead of becoming model
  turns. Real-PTY coverage checks alignment, filtering, selection, menu cleanup
  and the removed `/quit` alias.
- REPL startup banner shows model / skills / tools with aligned
  colored labels; per-turn status footer (tokens, elapsed ms, model);
  first-run command hints.
- REPL commands: `/sessions [TERM]` (review repl + gateway session
  files), `/resume FILE` (restore a validated full-turn snapshot),
  `/skills [TERM]`. Interactive turns checkpoint to
  `~/.local/share/motiris/sessions/`.
- Bounded full-turn JSON checkpoints for CLI `-r/--save`, REPL and gateway:
  complete user/assistant/tool-call/tool-result order, 1 MiB/256-message/
  256-KiB-record limits, ownership/integrity/pairing validation and atomic
  mode-0600 replacement. The host backend is excluded from the strict core
  no-filesystem probe and adds no core dependency.
- Portable `motiris_session_export` / `motiris_session_import` memory snapshots
  share the same bounded full-turn contract with the host-file backend. The
  caller owns exported RAM and supplies any persistence; strict-C11 nofs tests
  cover exact round-trip, ownership and corruption rejection without file I/O.
- Optional full-CLI checkpoint-backend cut (`PERSISTENCE=0`): explicit file
  save/resume operations fail closed with bounded no-I/O stubs, REPL automatic
  checkpoints are disabled, and gateway sessions report `volatile-only`
  without loading, writing or reset-unlinking snapshots. The portable core
  memory codec remains available; offline PTY/gateway integration runs in CI.
- Per-agent live-history item/byte retention and independent replay budgets.
  Complete oldest user turns are evicted without orphaning tool results;
  over-budget active turns fail closed. Environment/C-API limits are
  configurable, restore enforces live limits, and replay never mutates saved
  history. Strict-C11/no-filesystem and offline multi-turn bounds tests cover
  the portable core implementation.
- Skills: `skill_patch` / `skill_write` model tools to edit indexed
  `--skill-dir` files in place (frontmatter re-parsed on write).
- Goal check (`--goal-check` / config `"goal_check"`): after a first
  answer the model confirms the goal (first user message) with `DONE`
  or keeps working — one extra round, bounded by `--max-steps`.
- Offline smoke suite grown to 27 checks.
- Optional file-backend cut (`FILE_TOOLS=0`) with bounded unavailable-tool
  stubs, plus a strict-C11 no-POSIX-file-I/O core/echo portability probe and
  documented host/MCU validation boundary.
- Optional shell-backend cut (`SHELL_TOOLS=0`) compiles out and unregisters
  the host subprocess tool without changing the portable core or public API.
- Optional persistent-memory backend cut (`MEMORY_TOOLS=0`) replaces host
  JSON file I/O with bounded no-I/O `memory_get`/`memory_set` stubs. Offline
  fault-injection and model-driven tests cover atomic failure and corruption
  preservation, with a leak-enabled sanitizer CI gate.
- Optional host-backed `vision_analyze` (`VISION_TOOLS=1`): one bounded,
  workspace-local image to a trusted OpenAI-compatible vision provider, with
  explicit MIME, configurable input/timeout limits, bounded responses/text,
  local-mock integration tests and no default-core dependency or image runtime.
- Optional host-only REPL TTS adapter (`TTS_TOOLS=1`): final replies go to a
  trusted external engine over stdin, capped at 16 KiB with a monotonic
  timeout and process cleanup. Fake-engine tests cover fallback and bounds;
  default/core builds contain no adapter or audio dependency.

### Changed

- CI no longer enforces a binary-size ceiling. Smoke #4 records cold-start
  peak RSS instead, and memory integration records maximum-value peak RSS
  with GNU `time`; compatibility and Docker test images install the tool.

### Fixed

- Keyless provider-list entries no longer inherit the default Agent or
  `MOTIRIS_API_KEY` Bearer credential. Omitted/missing entry key environments,
  independent keys and mixed multi-hop fallback are covered by offline mock
  tests. The existing fixed 512-byte Authorization buffer now rejects keys
  over 489 bytes before network I/O rather than silently omitting
  authentication; errors never print the key.
- File and persistent-memory handlers reject missing or wrongly typed required
  strings before touching stored state. This closes missing-`content` /
  missing-`value` data erasure and uninitialized file-error reads. File reads
  and patches now enforce the 1 MiB regular-file bound without FIFO blocking;
  writes and patches use complete same-directory temporary writes,
  `fsync`/close checks and atomic rename, preserving old bytes on the listed
  pre-rename failures. Direct handler fault-injection suites run in tools and
  sanitizer CI.
- `browser_fetch` and `subagent` now share bounded host-child execution:
  `MOTIRIS_BROWSER_TIMEOUT_S` (default 30 s) and
  `MOTIRIS_SUBAGENT_TIMEOUT_S` (default 120 s), each range 1..120, apply one
  monotonic deadline to setup, output draining and exit. Combined output is
  drained/discarded after 64 KiB (browser) or 32 KiB (subagent), with exit,
  signal, timeout, truncation and bounded error metadata. Process-group
  TERM/grace/KILL/reap handles timeout and I/O failure, including leaders that
  exit before pipe-holding descendants; normal return also cleans same-group
  background members. Offline tests cover floods, continuous output, silent
  and TERM-ignoring children, descendant cleanup, repeated-call fd/RSS
  stability, invalid settings and successful calls after timeout.
- MCP stdio initialize/list/call and notification writes now share bounded
  monotonic request deadlines (`MOTIRIS_MCP_TIMEOUT_S`, default 10 s, range
  1..120). Nonblocking `poll` handles partial I/O and early server exit;
  oversized lines and JSON-RPC/transport failures return bounded diagnostics.
  Fatal failures close the connection without repeated waiting, while process
  cleanup uses a process-group TERM, one-second grace, KILL and direct-child
  reap. Offline tests cover silent initialization/list/call, blocked large
  writes, response caps, SIGPIPE, repeated cleanup and invalid configuration.
- `web_search` validates every DDG row delimiter before pointer arithmetic or
  copying. Malformed/truncated rows are skipped without NULL arithmetic or
  out-of-bounds reads; normal titles and bounded 511/200-byte field prefixes
  retain the 10-result cap. Direct parser regressions, including deterministic
  malformed-input fuzzing, run under ASAN/UBSAN.
- A singleton configured provider now uses its declared model, endpoint and
  key instead of silently falling back to the default provider. One-, two-
  and eight-entry offline routing tests verify selection and fallback order.
- Streamed REPL answers are no longer printed as both deltas and an aggregated
  duplicate. Suppression is tied to deltas actually delivered by the selected
  backend, so buffered/echo/spawned-curl fallback still prints exactly once.
  Goal-check `DONE` remains silent over SSE, and a provider that fails after
  visible deltas returns the error instead of mixing in a fallback answer.
- Provider response buffers use checked geometric growth and configured hard
  limits rather than unchecked, unbounded `realloc`; partial libcurl, SSE and
  spawned-curl state is freed on limit/allocation failures. Streamed tool-call
  indices are validated before casting, and authenticated spawned-curl argv /
  pipe/fork setup no longer has terminator/descriptor cleanup defects.
- REPL live slash-menu rows use carriage-return/newline pairs in raw terminal
  mode, keeping every command left-aligned instead of stair-stepping right.
- Successful SSE response trees and temporary unknown/empty-tool JSON strings
  are now freed after serialization. Leak-enabled ASAN runs cover both the
  interrupted-stream/follow-up path and the strict-C11 core retry path.
- Session restore no longer trusts ambiguous U/A/T rows: multiline model/tool
  content cannot forge later user turns, and assistant/tool state survives
  CLI/REPL/gateway restoration. Legacy, corrupt, oversized, mismatched and
  incomplete snapshots fail closed; failed turns preserve the last valid file.
- `shell` now drains stdout/stderr concurrently, caps per-stream and combined
  retained output, applies a monotonic timeout, and kills/reaps the command
  process group on timeout or execution errors. Model-visible results include
  truncation/signal/timeout diagnostics; regression tests cover the former
  128 KiB stderr deadlock, output floods, cleanup and maximum-cap RSS.
- Persistent memory no longer silently erases empty, malformed, non-object,
  oversized or incompletely read files. Only a missing file starts empty;
  updates use bounded 256-byte keys, 256 KiB values and 1 MiB files, complete
  mode-0600 temporary writes plus `fsync`/close checks and atomic rename.
  Failures preserve the last valid bytes, FIFOs and other non-regular files
  are rejected without blocking, and replaced cJSON values are fully freed
  rather than leaking on every overwrite.
- `web_fetch` / `web_search` receive buffers now cover every full libcurl
  chunk without integer overflow, out-of-bounds writes or `realloc` loss.
  Bodies retain at most 64 KiB plus a private NUL byte; excess is drained
  and discarded with an explicit `truncated` result. Real tool-call
  regressions cover chunk/cap boundaries under ASAN/UBSAN.
- Gateway request parsing no longer scans unterminated partial headers;
  malformed input gets `400`, headers over 16 KiB or bodies over 1 MiB
  get `413`, and receive-buffer allocation is bounded. Added segmented
  request regressions and an ASAN/UBSAN CI job.
- Gateway connection I/O now has configurable monotonic deadlines
  (`MOTIRIS_GATEWAY_IO_TIMEOUT_S`, default 30 s, range 1..120). Nonblocking
  poll/read/write loops bound complete request receives and response sends
  without resetting on trickle traffic or `EINTR`; receive expiry returns
  `408`, write expiry closes/logs the connection, and later health checks
  proceed. Half-open header/body, trickle, non-reading-client, repeated-cleanup
  and invalid-setting regressions run in the existing tools/sanitizer jobs.
- Gateway snapshots use injective hex filenames plus canonical `session_id`
  binding, preventing cross-chat history leaks after restart or eviction.
  Legacy sanitized/I-header logs are not guessed or merged; invalid ownership
  fails closed and exact-ID reset does not affect other chats.
- `search_files` no longer follows recursive symlinks outside the workspace
  or around directory loops. fd-relative no-follow opens, iterative traversal,
  depth/node/byte/line/result bounds and provider-round privacy regressions
  close the escape and unbounded-recursion paths.

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