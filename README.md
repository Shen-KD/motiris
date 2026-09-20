# hermote

**Hermes as a mote** — a featherweight C11 agent framework in the spirit
of [Hermes](https://github.com/NousResearch/hermes-agent) and
[DeepSeek Harness](https://github.com/deepseek-ai/deepseek-harness).

The name fuses **Hermes** (the messenger god, who carries the word between
worlds) with **mote** (a speck of dust). Hermes speaks for the gods;
hermote speaks for models — and weighs like a mote of dust. The compiled
binary is **~55 KB**, it has **zero runtime dependencies** (no Node, no
Python, no database, no web server), and the agent process stays
idle-quiet between requests. `Model + Harness = Agent` — but the harness
weighs nothing.

## Features

- **Agent loop with tool calling** — OpenAI-compatible chat completions,
  iterates `model -> tool_calls -> execute -> result -> model` until done.
  Works with OpenAI, DeepSeek, Ollama, or any OpenAI-compatible endpoint.
- **Runtime tool plug/unplug** — tools are registered in a registry;
  `hermote_unregister_tool()` removes them at runtime, `--no-tools` starts bare.
- **Runtime plugin system** — plugins are shared objects (`.so`) dropped
  into a plugins directory; hermote dlopens them at startup and they register
  tools through a small plugin API (like Harness's "everything is a plugin",
  minus the framework).
- **Sessions** — append-only trace log (`--save`), resume replay (`-r`).
- **Skills** — a directory of `.md` files injected as system context.
- **Tiny core** — cJSON is the only vendored dependency; HTTP goes through
  a short-lived `curl` child process (HTTPS/TLS handled by curl, the agent
  itself never links a network stack).

## Architecture

Designed by cribbing the good parts of Hermes and DeepSeek Harness and
cutting everything that needs a heavy runtime:

```
                 +-------------------------------+
                 |  hermote CLI  (one 55 KB binary)  |
                 +-------------------------------+
 Loops     agent loop: request -> tool_calls -> execute -> result
 Models    one provider: OpenAI-compatible HTTP (multi-vendor by choice)
 Tools     registry with runtime plug/unplug, incl. shell/time built-ins
 Plugins   .so files, dlopen'd, register tools via MotePluginApi  <-- plug
 Skills    --skill-dir *.md injected as system prompt
 Sessions  append-only JSON trace + resume replay (no database)
 Storage   out of the way: only what you ask for (--save)
 Trace     -v step/tool logging to stderr (append-only spirit)
 Sandbox   none by default: shell runs as your uid (documented risk)
           [roadmap] subagents, scheduling, SSE streaming, MCP
```

Compared to the reference frameworks: Hermes's gateway/plugins/cron and
Harness's sandbox/UI/scheduling are explicitly out of scope for v1 — the
plugin API is the seam where they plug back in.

## Build

```
make            # cc -O2, single binary ./hermote (stripped)
make test       # offline smoke tests, no API key required
```

Requires: a C11 compiler, `curl` on PATH at runtime, and `-ldl`
(POSIX dlopen; glibc ≥ 2.34 needs no extra package).

## Usage

```
hermote -p "summarize this repo" -m deepseek-chat -k "$DEEPSEEK_API_KEY"
printf 'what time is it' | hermote                      # prompt from stdin
hermote -p hi --transport echo                          # offline demo
hermote -p "review diff" -s "You are a code reviewer" -v
hermote -p "continue" -r last-run.log                   # resume a session
hermote -p "do X" --skill-dir ./skills                  # skills as context
```

Environment: `HERMOTE_API_KEY`, `HERMOTE_MODEL`, `HERMOTE_PLUGIN_DIR`, `HERMOTE_CURL`.
Default endpoint: `https://api.openai.com/v1/chat/completions`.

Built-in tools (disable with `--no-tools`):

- `shell(command)` — run `/bin/sh -c`, returns stdout/stderr/exit_code.
  **Your uid, no sandbox. Use with care.**
- `time()` — UTC ISO-8601 + unix seconds.

## Writing a tool (compile-time)

```c
#include "hermote.h"
static char *count_call(const char *args, void *ud) { /* return JSON */ }
MoteTool t = { "count", "Count things.", "{\"type\":\"object\"}", count_call, NULL };
hermote_register_tool(agent, &t);   /* hermote_unregister_tool(agent, "count") to unplug */
```

## Writing a plugin (runtime plug)

Build any tool into a shared object exporting `hermote_plugin_init` and drop
it into a plugins dir — see `examples/hello_plugin.c` for the full example:

```c
int hermote_plugin_init(MoteAgent *a, const MotePluginApi *api) {
    MoteTool t = { ... };
    api->register_tool(a, &t);
    return 0;
}
```

```
make examples/hello_plugin.so
mkdir -p ~/.local/share/hermote/plugins && cp examples/hello_plugin.so ~/.local/share/hermote/plugins/
hermote -p "say hello to moto"     # plugin tool appears to the model automatically
```

## Layout

```
include/hermote.h        public API: agent, tools, plugin ABI
src/agent.c           agent loop, request assembly, tool dispatch
src/transport.c       curl child / echo backends
src/tools.c           built-in tools (shell, time)
src/plugin.c          .so loader (HERMOTE_PLUGIN_DIR, --plugin-dir)
src/main.c            CLI, sessions, skills
src/vendor/cJSON.c/h  vendored cJSON (MIT)
examples/             plugins, extensions
tests/smoke.sh        offline test suite (make test)
```

## License

MIT. cJSON (vendored) is MIT © Dave Gamble et al.