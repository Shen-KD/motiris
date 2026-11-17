# motiris

**Iris as a mote** — a featherweight C11 agent framework.

## Quick install (from GitHub releases)

```
sh install.sh                 # latest release -> $HOME/bin (or ~/.local/bin)
sh install.sh --skip-config   # binary only, keep your own config
# knobs: --prefix DIR --model NAME --base-url URL --api-key-env VAR
```

Downloads the latest release asset, verifies its sha256, installs the
binary, and writes `~/.motiris/{env,config.json}` (never overwrites an
existing config). Build from source instead: `make && make install`.

The name fuses **Iris** (Ἶρις, goddess of the rainbow and messenger of
the gods, who carried the word across the bridge between worlds) with
**mote** (a speck of dust). Motiris speaks for models — and weighs like a
mote of dust. The compiled binary is **~55 KB**, it has **zero runtime
dependencies** (no Node, no Python, no database, no web server), and the
agent process stays idle-quiet between requests.
`Model + Harness = Agent` — but the harness weighs nothing.

## Features

- **Agent loop with tool calling** — OpenAI-compatible chat completions:
  iterates `model -> tool_calls -> execute -> result -> model` until done.
  Works with any OpenAI-compatible endpoint (cloud or local). With
  `--goal-check`, after a first answer the model is asked whether the
  goal (the first user message) is fully met: it replies `DONE` to
  finish, or keeps working (tools included), bounded by `--max-steps`.
- **Runtime tool plug/unplug** — tools live in a registry;
  `motiris_unregister_tool()` removes one at runtime, `--no-tools` starts bare.
- **Runtime plugin system** — plugins are shared objects (`.so`) dropped
  into a plugins directory; motiris dlopens them at startup and they
  register tools through a small plugin API. Add a tool without touching
  the core binary.
- **Optional vision tool** — `VISION_TOOLS=1` adds workspace-bounded image
  input to a configured remote vision provider and returns bounded text.
  The default build contains no vision code and no image inference runtime.
- **Optional host TTS** — `TTS_TOOLS=1` lets the REPL pass each final reply
  to one trusted external speech engine, with bounded input and timeout.
  Text remains available on engine failure; the default build has no TTS code.
- **Full-turn sessions** — bounded, versioned JSON checkpoints (`--save`,
  `-r`) preserve complete user/assistant/tool-call/tool-result messages.
  Portable core memory export/import and an optional atomic host-file backend
  share ownership/integrity checks and strict legacy rejection; `/sessions`
  remains the review list.
- **Bounded live history and replay** — per-agent item/byte budgets evict only
  complete oldest user turns. Independent replay limits select a safe suffix
  for the provider without changing stored history or splitting tool calls.
- **Skills** — a directory of `.md` files injected as system context.
- **Tiny core** — cJSON is the only runtime-embedded vendor; HTTP goes
  through a built-in libcurl C backend (no child process; HTTPS/TLS
  handled by the system libcurl shared library). A spawn-`curl` fallback
  exists for hosts without libcurl, and an offline `echo` backend for
  tests.

## Architecture

```
                 +-------------------------------+
                 |  motiris CLI (one ~55 KB binary) |
                 +-------------------------------+
 Loops     agent loop: request -> tool_calls -> execute -> result
 Models    one provider: OpenAI-compatible HTTP (multi-vendor by choice)
 Tools     registry with runtime plug/unplug, incl. shell/time built-ins
 Plugins   .so files, dlopen'd, register tools via MotirisPluginApi
 Skills    --skill-dir *.md injected as system prompt
 Sessions  bounded full-turn JSON checkpoints (no database);
          repl saves to ~/.local/share/motiris/sessions/
 Storage   out of the way: only what you ask for (--save)
 Trace     -v step/tool logging to stderr
 Sandbox   none by default: shell runs as your uid (documented risk)
```

Design principles: one small binary, one clear loop, everything else
pluggable. Heavy machinery (web UI, scheduling, sandboxes, subagents)
stays out of the core — the plugin API is the seam where it plugs back in.

## Build

```
make                  # cc -O2, single binary ./motiris (stripped)
make test             # offline smoke tests, no API key required
make FILE_TOOLS=0     # cut the POSIX file backend; bounded stubs remain
make SHELL_TOOLS=0    # compile out and unregister the host shell backend
make MEMORY_TOOLS=0   # cut persistent memory files; bounded stubs remain
make PERSISTENCE=0    # cut host checkpoint files; REPL/gateway use RAM only
make test-nofs        # strict-C11 core + file/memory/shell backend cut test
make test-persistence # full-CLI volatile-only checkpoint backend cut test
make test-core-nofs   # core-only retention + memory snapshot round-trip
make test-shell       # bounded shell execution integration tests
make test-memory      # memory corruption/atomic-failure/replacement tests
make test-vision      # clean optional-vision build + local mock integration
make VISION_TOOLS=1   # opt in to the host-backed vision_analyze tool
make TTS_TOOLS=1      # opt in to host-only REPL speech output
make test-tts         # optional TTS tests with a fake engine, no audio device
```

Requires: a C11 compiler and the libcurl shared library at runtime
(libcurl dev headers are vendored in `deps/libcurl/`, pinned to the
curl 8.18 ABI). No other dependencies. `FILE_TOOLS=0` replaces only the
optional file tools with fixed-error stubs. `SHELL_TOOLS=0` independently
compiles out the `fork`/`pipe`/`poll`/`waitpid` backend and leaves `shell`
unregistered; the `time` tool remains. `MEMORY_TOOLS=0` independently replaces
the persistent JSON memory backend with fixed-error `memory_get`/`memory_set`
stubs that perform no file I/O. Other host tools and the CLI still use their
documented OS facilities. `PERSISTENCE=0` independently replaces only
the host full-turn checkpoint file backend with fixed-error stubs: explicit
`--save`, `-r` and `/resume` fail closed without touching the supplied path,
REPL automatic checkpoints are disabled, and the gateway reports
`checkpoint: volatile-only` while retaining bounded in-process history. The
portable core memory export/import API and unrelated memory tools remain.
`VISION_TOOLS=1` is a separate opt-in with its own POSIX file and libcurl
network backend; it remains excluded when `FILE_TOOLS=0` unless both switches
are explicitly enabled. Run `make clean` when changing compile-time switches.
`make test-core-nofs` separately builds
the core loop/config/echo transport and memory snapshot codec as strict C11
without POSIX file I/O or libcurl. That core-only probe is not a claim of
MCU/RTOS support; see [docs/DEVELOPING.md](docs/DEVELOPING.md) for the
platform matrix and bounds.

## Usage

```
motiris -p "summarize this repo" -m deepseek-chat -k "$DEEPSEEK_API_KEY"
printf 'what time is it' | motiris                      # prompt from stdin
motiris -p hi --transport echo                          # offline demo
motiris -p "review diff" -s "You are a code reviewer" -v
motiris -p "do X" --goal-check                          # confirm goal met before finishing
motiris -p "continue" -r last-run.log                   # resume a session
motiris -p "do X" --skill-dir ./skills                  # skills as context
motiris --gateway :8899                                 # HTTP gateway
curl -X POST localhost:8899/v1/chat -d '{"chat_id":"dev","message":"hi"}'
motiris --cron jobs.json --once                          # scheduled jobs
motiris --sessions [TERM]                                # list/search logs
# repl (-i): type / for a live prefix-filtered command list; /exit to leave
# commands: /help /new /tools /sessions [TERM] /resume FILE /skills [TERM] /exit
# Ctrl+C during a reply cancels generation, keeps the user instruction,
# discards partial assistant/tool messages, and accepts a follow-up instruction
# interactive repl turns checkpoint to ~/.local/share/motiris/sessions/
# token-protected: MOTIRIS_GATEWAY_TOKEN=sekret motiris --gateway :8899
```

The gateway keeps one context per `chat_id` (session logs survive
restarts under `~/.local/share/motiris/gateway/`), exposes
`GET /health` and `POST /v1/chat` (`{"chat_id","message","reset"?}`),
and optionally requires `Authorization: Bearer $MOTIRIS_GATEWAY_TOKEN`.
Requests are limited to 16 KiB of headers and a 1 MiB body; malformed or
incomplete requests get `400`, while oversized requests get `413` and the
connection is closed. `chat_id` is limited to 120 UTF-8 bytes. Its snapshot
filename is the lowercase hex encoding of the complete ID (at most 244 bytes
including `.log`), so punctuation, case, Unicode and long IDs cannot alias.
Each snapshot also binds that encoded ID in `session_id`; mismatches fail
before history reaches the provider. Legacy I/U/A/T logs and former sanitized
filenames are not migrated or replayed. `"reset":true` removes only that exact
ID's snapshot and starts fresh. Successful responses report `checkpoint` as
`saved` or `volatile-only`, or include a bounded `checkpoint_error`.

Each agent's live history is capped at 256 messages and 16 MiB of compact
serialized message records by default. `MOTIRIS_HISTORY_MAX_MESSAGES` and
`MOTIRIS_HISTORY_MAX_BYTES` lower those limits (also available through the C
API). When a new record would exceed either budget, complete oldest user turns
are evicted first; assistant/tool sequences are never split. If the active
turn alone cannot fit, the run fails with an explicit diagnostic instead of
truncating content or growing without bound. The 16 MiB ceiling accommodates
existing bounded host-tool results, is not preallocated, and is not an MCU RAM
budget; embedded users should select a target-appropriate limit.

Provider replay has independent `MOTIRIS_REPLAY_MAX_MESSAGES` and
`MOTIRIS_REPLAY_MAX_BYTES` limits (the same defaults). Replay includes the
whole active turn plus as many newest complete older turns as fit. A smaller
replay budget does not delete live or persisted history; an over-budget active
turn fails before any provider request. Restore applies the live limits before
reporting the number of retained messages.

CLI `--save`, interactive REPL turns and gateway turns atomically replace one
latest snapshot per session. The host backend limits files to 1 MiB, snapshots
to 256 messages and 256 tool calls, and each serialized message to 256 KiB.
It preserves message order and multiline text as JSON, validates tool
call/result pairing and an integrity digest, and writes a mode-0600 temporary
file followed by flush/`fsync` and atomic `rename`. The FNV-1a-64 digest is
for corruption detection, not cryptographic authentication. Corrupt, oversized,
version-mismatched and legacy input fails closed; restoring never executes an
old tool. REPL `/resume` requires an empty context (use `/new` first). The
backend is in the host harness and is absent from the strict core-only
no-filesystem probe. It defaults to `PERSISTENCE=1`; `PERSISTENCE=0` compiles
out its stdio/`fsync`/atomic-`rename` implementation. In that full-CLI cut,
explicit file operations return `snapshot persistence unavailable`, TTY REPL
turns do not auto-save, and gateway sessions neither load nor write snapshots
(including reset unlinking); successful gateway replies remain
`volatile-only`. No MCU/RTOS backend is claimed.

The portable core also exposes `motiris_session_export` and
`motiris_session_import` for caller-owned memory buffers. Export returns one
malloc'd, NUL-terminated snapshot of at most 1 MiB; import takes an explicit
length excluding that readable NUL terminator. The core retains no exported
copy and provides no storage service: an embedder can keep the buffer in RAM
or supply its own bounded RTOS/HAL persistence. Memory round-trip, ownership
and corruption rejection run in the strict core-only probe without host file
I/O. This is not durable storage by itself and is not an MCU hardware claim.

Environment: `MOTIRIS_API_KEY`, `MOTIRIS_MODEL`, `MOTIRIS_BASE_URL`,
`MOTIRIS_PLUGIN_DIR`, `MOTIRIS_TOOLS_DIR`, `MOTIRIS_CURL`,
`MOTIRIS_GATEWAY_TOKEN`, `MOTIRIS_VISION_MODEL`,
`MOTIRIS_VISION_BASE_URL`, `MOTIRIS_VISION_API_KEY`,
`MOTIRIS_VISION_MAX_BYTES`, `MOTIRIS_VISION_TIMEOUT_S`,
`MOTIRIS_TTS_CMD`, `MOTIRIS_TTS_TIMEOUT_S`,
`MOTIRIS_SHELL_TIMEOUT_S`, `MOTIRIS_SHELL_STDOUT_MAX`,
`MOTIRIS_SHELL_STDERR_MAX`, `MOTIRIS_SHELL_OUTPUT_MAX`,
`MOTIRIS_HISTORY_MAX_MESSAGES`, `MOTIRIS_HISTORY_MAX_BYTES`,
`MOTIRIS_REPLAY_MAX_MESSAGES`, `MOTIRIS_REPLAY_MAX_BYTES`. Default endpoint:
`https://api.openai.com/v1/chat/completions`.

Built-in tools (disable with `--no-tools`):

- `shell(command)` — run `/bin/sh -c`, returning bounded stdout/stderr plus
  `exit_code`, `signal`, `timed_out` and overall/per-stream truncation flags.
  One monotonic deadline covers process setup, execution and pipe draining.
  `MOTIRIS_SHELL_TIMEOUT_S` defaults to 30 s (allowed 1..120);
  `MOTIRIS_SHELL_STDOUT_MAX` / `MOTIRIS_SHELL_STDERR_MAX` default to 16,384
  retained bytes per stream and `MOTIRIS_SHELL_OUTPUT_MAX` to 32,768 bytes
  combined. Each output limit accepts 1..262,144 bytes. Excess output is
  drained and discarded with truncation flags; an over-deadline process group
  is killed and the direct child is reaped. Invalid limits fail before `fork`.
  **Your uid, no sandbox. Use with care.** The execution budget is not a
  security boundary. `SHELL_TOOLS=0` removes this host backend at build time.
- `time()` — UTC ISO-8601 + unix seconds.

File tools (workspace = `MOTIRIS_WORKSPACE` or cwd; paths outside it are
rejected):

- `read_file(path)` / `write_file(path, content)` / `patch(path, old, new)`
- `search_files(pattern, path?)` — bounded recursive regex grep. Recursive
  symlinks are skipped; every component is opened relative to the workspace
  with `O_NOFOLLOW`. Limits: 32 directory levels, 4096 entries, 1 MiB per
  file / 16 MiB total, 4096-byte line buffer and 200 matches. Results include
  skip/truncation diagnostics. Explicit start paths must still canonicalize
  inside the workspace.

Knowledge & delegation tools:

- `web_fetch(url)` — libcurl GET, HTML stripped to text; retains at most
  64 KiB, drains/discards excess bytes and reports `"truncated": true`
- `web_search(query)` — DuckDuckGo lite (keyless), titles+links; uses the
  same 64 KiB response cap and truncation flag
- `memory_set(key, value)` / `memory_get(key?)` — persistent JSON memory
  (`~/.local/share/motiris/memory.json`). Keys are limited to 256 bytes, values
  to 256 KiB and the file to 1 MiB. Only a missing file starts empty: empty,
  malformed, non-object, oversized, non-regular or incompletely read files
  return a bounded error and are preserved byte-for-byte. Updates write a
  mode-0600 same-directory temporary file, check write/`fsync`/close and
  atomically rename it; failures preserve the last valid file. Concurrent
  multi-process read/modify/write locking is not provided. `MEMORY_TOOLS=0`
  compiles out this host backend and leaves no-I/O stubs.
- `skill_list()` / `skill_load(name)` — SKILL.md frontmatter index,
  on-demand loading (`--skill-dir DIR`)
- `skill_patch(name, old, new)` / `skill_write(name, content)` —
  edit indexed skill files in place (only files inside --skill-dir)
- `subagent(task)` — run a fresh `motiris` child (no tools, isolated)
- `browser_fetch(url)` — render via headless chromium (`--dump-dom`),
  needs a chromium binary; `MOTIRIS_BROWSER` overrides

Optional host-backed tool (only with `VISION_TOOLS=1`):

- `vision_analyze(path, mime, question?)` — read one non-empty regular image
  inside `MOTIRIS_WORKSPACE` (or cwd), encode it as a data URL, and send one
  text + `image_url` request to an OpenAI-compatible provider. JPEG, PNG, GIF
  and WebP MIME labels are accepted. Input defaults to 256 KiB and is hard-
  capped at 1 MiB (`MOTIRIS_VISION_MAX_BYTES`); total request timeout defaults
  to 30 s and is capped at 120 s (`MOTIRIS_VISION_TIMEOUT_S`). Provider
  responses are capped at 64 KiB and returned descriptions at 16 KiB.
  `MOTIRIS_VISION_MODEL`, `MOTIRIS_VISION_BASE_URL` and
  `MOTIRIS_VISION_API_KEY` override the agent's model/endpoint/key. Images
  leave the device: configure only a trusted provider. Redirects are not
  followed; errors return bounded JSON and the outer agent can continue in
  text. This is not local inference, video, generation, or MCU support.

Optional host-only REPL speech output (only with `TTS_TOOLS=1`):

- Set `MOTIRIS_TTS_CMD` to a trusted executable. After each successful REPL
  turn, motiris writes the final assistant text to that program's stdin and
  closes it. The program can synthesize, play, or save audio; it receives no
  shell command line and its stdout/stderr are discarded. For example, a small
  wrapper script can add the options required by a locally installed engine.
- Input is limited to 16,384 bytes per turn. Longer replies remain fully
  visible as text, but only that prefix reaches the engine. The adapter owns
  no audio buffer and retains no text between turns. The external engine is
  responsible for its own audio storage and resource limits.
- `MOTIRIS_TTS_TIMEOUT_S` bounds each invocation (default 10, range 1..120).
  Timeout, missing/failing engines, and invalid settings produce a bounded
  diagnostic without hiding the reply or failing the REPL turn. Goal-check's
  synthetic `DONE` acknowledgement is not spoken.
- With no `MOTIRIS_TTS_CMD`, the optional runtime path is idle and starts no
  processes. This is REPL-only, not STT, streaming/full-duplex audio, local
  neural inference, gateway audio, or MCU/RTOS support.

Edit scalar JSON config without a text editor:

```sh
motiris set config model glm-5.3
motiris set config stream false       # JSON boolean, not a string
motiris set config max_steps 20       # JSON integer
motiris set config api_key_env MOTIRIS_API_KEY
```

The command updates `$MOTIRIS_HOME/config.json` (default
`~/.motiris/config.json`) without starting an agent or contacting a provider.
String keys are `model`, `base_url`, `api_key_env`, `transport`, `system`,
`plugin_dir`, `shell_allow` and `shell_deny`; boolean keys are `stream`,
`tools`, `plugins` and `goal_check` (`true`/`false`). `max_steps` must be a
positive integer. Other fields, including `providers`, `mcp_servers` and
extension fields, are preserved. Writes use a complete mode-0600 temporary
file plus `fsync` and atomic rename. Empty, malformed, non-object, oversized
or non-regular input is rejected without resetting the original; invalid
keys/values and failed writes also leave it unchanged. Values are not printed.
There is deliberately no plaintext `api_key` key: store secrets separately
in the mode-0600 env file and reference them with `api_key_env`.

MCP servers (`"mcp_servers": [{"name","cmd","args":[]}]` in config.json)
register each remote tool as `<server>:<tool>`; `tools/call` is bridged
over stdio JSON-RPC.

Shell safety policy: config `"shell_allow": "git,ls"` /
`"shell_deny": "rm,sudo"` (prefix match, deny wins) is enforced in the
shell tool.

## Writing a tool (compile-time)

```c
#include "motiris.h"
static char *count_call(const char *args, void *ud) { /* return JSON */ }
MotirisTool t = { "count", "Count things.", "{\"type\":\"object\"}", count_call, NULL };
motiris_register_tool(agent, &t);  /* motiris_unregister_tool(agent, "count") to unplug */
```

## Writing a plugin (runtime plug)

Build any tool into a shared object exporting `motiris_plugin_init` and
drop it into a plugins dir — see `examples/hello_plugin.c`:

```c
int motiris_plugin_init(MotirisAgent *a, const MotirisPluginApi *api) {
    MotirisTool t = { ... };
    api->register_tool(a, &t);
    return 0;
}
```

```
make examples/hello_plugin.so
mkdir -p ~/.motiris/plugins/hello
cp examples/hello_plugin.so ~/.motiris/plugins/hello/hello.so
motiris -p "say hello to iris"   # plugin tool appears to the model automatically
```

Per-tool directory layout (tools and plugins share it):
`<dir>/<name>/{<name>.so, <name>.h, <name>.json}` — the `.json` is the
tool's JSON-Schema (loaded at startup, overrides the embedded one when
present, invalid files are skipped with a warning), `.h` is the
compile-time contract, `.a` (optional) for static linking. Legacy flat
`<name>.so` plugins still load. Built-in tools use the same override
path via `MOTIRIS_TOOLS_DIR` (default `~/.motiris/tools`): dropping
`shell/shell.json` there (see `examples/shell/`) changes the shell
tool's schema without rebuilding.

## Development

Working notes for this repo (layout, build/test loop, extension
points, trap log): [docs/DEVELOPING.md](docs/DEVELOPING.md). Release
history: [CHANGELOG.md](CHANGELOG.md).

## Layout

```
include/motiris.h      public API: agent, tools, plugin ABI
src/agent.c            agent loop, request assembly, tool dispatch
src/core/checkpoint.c  portable bounded memory snapshot export/import
src/transport.c        libcurl C backend (default) / spawn-curl / echo
src/tools.c            built-in tools (shell, time)
src/filetools.c        read/write/patch/search (workspace-guarded)
src/webtools.c         web_fetch / web_search
src/memorytools.c      persistent memory tools
src/skilltools.c       SKILL.md index + on-demand load
src/subagent.c         child-agent delegation tool
src/browser.c          headless chromium fetch
src/tools/visiontools.c optional bounded vision provider tool
src/mcp.c              MCP stdio client (JSON-RPC)
src/gateway.c          embedded HTTP gateway (session-per-chat_id)
src/harness/checkpoints_nofs.c fixed-error stubs for PERSISTENCE=0
src/cron.c             scheduled agent runs
src/plugin.c           .so loader (MOTIRIS_PLUGIN_DIR, --plugin-dir)
src/main.c             CLI parsing (switch), --sessions, modes
src/vendor/cJSON.c/h   vendored cJSON (MIT)
deps/libcurl/include/  vendored libcurl dev headers (curl 8.18 ABI)
examples/              plugins, extensions
tests/smoke.sh         offline test suite (make test)
Dockerfile.test        isolated test image (make test-docker)
```

## Testing

```
make test          # 27 default offline smoke checks, no network/key needed
make test-core-nofs # strict-C11 core + caller-owned memory snapshots
make test-persistence # PERSISTENCE=0 fail-closed/volatile-only integration
make test-shell    # shell dual-stream/cap/timeout/reaping regression cases
make test-memory   # memory corruption/atomic faults + replacement coverage
make test-vision   # optional vision build + 10 local-mock integration cases
make test-tts      # optional TTS build + 8 fake-engine integration cases
make test-docker   # default suite inside a throwaway debian container
                   # (clean HOME/ports/processes, mirrors CI)
```

Test image: `gcc libc6-dev make libcurl4 python3 curl time` on
debian:bookworm-slim — mock LLM/MCP servers run inside the container, so
nothing leaks onto the host.

## License

MIT. cJSON (vendored) is MIT (c) Dave Gamble et al.

## CI & branch protection

GitHub Actions (`.github/workflows/ci.yml`) runs on every push/PR: gcc
build, offline smoke tests with runtime RSS reporting, clang `-Werror`
build, plugin example compile, and binary artifact upload. Binary size is
not a CI gate; `tests/perf.sh` and the memory integration tests record RAM
for ongoing optimization. The job is named `ci` — that exact name is the
required status check on `main`.

`main` is protected: PR-only (no direct pushes, no force push, no
deletions), requires the `ci` check to pass. Solo developers can still
merge their own PRs. Local layer: a pre-push hook blocks accidental
direct pushes to main:

```
git config core.hooksPath .githooks     # enable hook (already set for dev)
git checkout -b feat/foo && ... && git push -u origin HEAD
gh pr create --base main --head feat/foo --title "..." --body "..."
gh pr merge --squash --delete-branch
```