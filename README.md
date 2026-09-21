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
  Works with any OpenAI-compatible endpoint (cloud or local).
- **Runtime tool plug/unplug** — tools live in a registry;
  `motiris_unregister_tool()` removes one at runtime, `--no-tools` starts bare.
- **Runtime plugin system** — plugins are shared objects (`.so`) dropped
  into a plugins directory; motiris dlopens them at startup and they
  register tools through a small plugin API. Add a tool without touching
  the core binary.
- **Sessions** — append-only trace log (`--save`), resume replay (`-r`);
  the repl auto-logs every interactive session to
  `~/.local/share/motiris/sessions/` (gateway format) with
  `/sessions`, `/resume` review commands.
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
 Sessions  append-only trace + resume replay (no database);
          repl auto-logs to ~/.local/share/motiris/sessions/
 Storage   out of the way: only what you ask for (--save)
 Trace     -v step/tool logging to stderr
 Sandbox   none by default: shell runs as your uid (documented risk)
```

Design principles: one small binary, one clear loop, everything else
pluggable. Heavy machinery (web UI, scheduling, sandboxes, subagents)
stays out of the core — the plugin API is the seam where it plugs back in.

## Build

```
make            # cc -O2, single binary ./motiris (stripped)
make test       # offline smoke tests, no API key required
```

Requires: a C11 compiler and the libcurl shared library at runtime
(libcurl dev headers are vendored in `deps/libcurl/`, pinned to the
curl 8.18 ABI). No other dependencies.

## Usage

```
motiris -p "summarize this repo" -m deepseek-chat -k "$DEEPSEEK_API_KEY"
printf 'what time is it' | motiris                      # prompt from stdin
motiris -p hi --transport echo                          # offline demo
motiris -p "review diff" -s "You are a code reviewer" -v
motiris -p "continue" -r last-run.log                   # resume a session
motiris -p "do X" --skill-dir ./skills                  # skills as context
motiris --gateway :8899                                 # HTTP gateway
curl -X POST localhost:8899/v1/chat -d '{"chat_id":"dev","message":"hi"}'
motiris --cron jobs.json --once                          # scheduled jobs
motiris --sessions [TERM]                                # list/search logs
# repl (-i) commands: /help /new /tools /sessions [TERM] /resume FILE /skills [TERM]
# repl sessions auto-log to ~/.local/share/motiris/sessions/ (U/A/T rows)
# token-protected: MOTIRIS_GATEWAY_TOKEN=sekret motiris --gateway :8899
```

The gateway keeps one context per `chat_id` (session logs survive
restarts under `~/.local/share/motiris/gateway/`), exposes
`GET /health` and `POST /v1/chat` (`{"chat_id","message","reset"?}`),
and optionally requires `Authorization: Bearer $MOTIRIS_GATEWAY_TOKEN`.

Environment: `MOTIRIS_API_KEY`, `MOTIRIS_MODEL`, `MOTIRIS_BASE_URL`,
`MOTIRIS_PLUGIN_DIR`, `MOTIRIS_TOOLS_DIR`, `MOTIRIS_CURL`,
`MOTIRIS_GATEWAY_TOKEN`. Default endpoint:
`https://api.openai.com/v1/chat/completions`.

Built-in tools (disable with `--no-tools`):

- `shell(command)` — run `/bin/sh -c`, returns stdout/stderr/exit_code.
  **Your uid, no sandbox. Use with care.**
- `time()` — UTC ISO-8601 + unix seconds.

File tools (workspace = `MOTIRIS_WORKSPACE` or cwd; paths outside it are
rejected):

- `read_file(path)` / `write_file(path, content)` / `patch(path, old, new)`
- `search_files(pattern, path?)` — recursive regex grep, returns matches.

Knowledge & delegation tools:

- `web_fetch(url)` — libcurl GET, HTML stripped to text
- `web_search(query)` — DuckDuckGo lite (keyless), titles+links
- `memory_set(key, value)` / `memory_get(key?)` — persistent JSON memory
  (`~/.local/share/motiris/memory.json`)
- `skill_list()` / `skill_load(name)` — SKILL.md frontmatter index,
  on-demand loading (`--skill-dir DIR`)
- `skill_patch(name, old, new)` / `skill_write(name, content)` —
  edit indexed skill files in place (only files inside --skill-dir)
- `subagent(task)` — run a fresh `motiris` child (no tools, isolated)
- `browser_fetch(url)` — render via headless chromium (`--dump-dom`),
  needs a chromium binary; `MOTIRIS_BROWSER` overrides

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
src/transport.c        libcurl C backend (default) / spawn-curl / echo
src/tools.c            built-in tools (shell, time)
src/filetools.c        read/write/patch/search (workspace-guarded)
src/webtools.c         web_fetch / web_search
src/memorytools.c      persistent memory tools
src/skilltools.c       SKILL.md index + on-demand load
src/subagent.c         child-agent delegation tool
src/browser.c          headless chromium fetch
src/mcp.c              MCP stdio client (JSON-RPC)
src/gateway.c          embedded HTTP gateway (session-per-chat_id)
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
make test          # offline smoke suite (13 checks, no network/key needed)
make test-docker   # same suite inside a throwaway debian container
                   # (clean HOME/ports/processes, mirrors CI)
```

Test image: `gcc libc6-dev make libcurl4 python3 curl` on
debian:bookworm-slim — mock LLM/MCP servers run inside the container, so
nothing leaks onto the host.

## License

MIT. cJSON (vendored) is MIT (c) Dave Gamble et al.

## CI & branch protection

GitHub Actions (`.github/workflows/ci.yml`) runs on every push/PR: gcc
build, offline smoke tests, clang `-Werror` build, plugin example
compile, binary size check, and uploads the signed binary as an
artifact. The job is named `ci` — that exact name is the required status
check on `main`.

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