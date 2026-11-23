---
name: motiris-context-compaction
description: Install, enable, configure, and verify deterministic bounded context compaction.
---

# Motiris context compaction

Use this Skill when the user asks about long context, compaction, summaries, or
`context_compaction` configuration.

## Configuration

Keep compaction disabled unless the plugin is installed. Enable it with:

```sh
motiris --config context_compaction true
```

Settings in `config.json`:

- `compaction_threshold`: compact when history exceeds this message count (4..256).
- `compaction_keep`: minimum newest messages retained (1..128, less than threshold).
- `compaction_max_chars`: maximum generated summary bytes (256..65536).

The deterministic plugin summarizes only old complete turns, keeps the active
user turn and complete tool-call/result pairs, and never calls the model. If the
plugin is missing or compaction fails, the run stops without discarding context.

Verify with `tests/context_compaction.py`, then `tests/smoke.sh`. Update this
Skill whenever compaction behavior, configuration, or installation changes.
