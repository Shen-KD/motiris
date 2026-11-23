#!/bin/sh
# Install a verified motiris release, built-in defaults, and optional plugins.
set -eu

MOTIRIS_REPO="${MOTIRIS_REPO:-Shen-KD/motiris}"
MOTIRIS_BASE_URL="${MOTIRIS_BASE_URL:-https://token.moi.matrixorigin.cn/v1/chat/completions}"
MOTIRIS_MODEL="${MOTIRIS_MODEL:-deepseek-v4-flash}"
MOTIRIS_API_KEY_ENV="${MOTIRIS_API_KEY_ENV:-MOI_TAAS_API_KEY}"
MOTIRIS_SKIP_CONFIG="${MOTIRIS_SKIP_CONFIG:-0}"
MOTIRIS_PREFIX="${MOTIRIS_PREFIX:-}"
MOTIRIS_PLUGINS="${MOTIRIS_PLUGINS:-ask}"

say() { printf '\033[1;36mmotiris\033[0m %s\n' "$*"; }
die() { printf '\033[31mmotiris install: %s\033[0m\n' "$*" >&2; exit 1; }

usage() {
  cat <<'EOF'
usage: install.sh [--prefix=DIR] [--skip-config] [--base-url=URL]
                  [--model NAME] [--api-key-env VAR]
                  [--plugins=context-compaction|none]

Installs the latest verified release. Session, read/write, MCP and approval are
built in and need no plugin. Interactive installs offer the optional
context-compaction plugin; non-interactive installs default to no optional
plugins unless --plugins or MOTIRIS_PLUGINS selects one.

Env: MOTIRIS_REPO, MOTIRIS_PREFIX, MOTIRIS_BASE_URL, MOTIRIS_MODEL,
MOTIRIS_API_KEY_ENV, MOTIRIS_SKIP_CONFIG, MOTIRIS_PLUGINS.
EOF
}

for arg in "$@"; do
  case "$arg" in
    --help|-h) usage; exit 0 ;;
    --prefix=*) MOTIRIS_PREFIX="${arg#*=}" ;;
    --skip-config) MOTIRIS_SKIP_CONFIG=1 ;;
    --base-url=*) MOTIRIS_BASE_URL="${arg#*=}" ;;
    --model=*) MOTIRIS_MODEL="${arg#*=}" ;;
    --api-key-env=*) MOTIRIS_API_KEY_ENV="${arg#*=}" ;;
    --plugins=*) MOTIRIS_PLUGINS="${arg#*=}" ;;
    *) die "unknown argument: $arg (see --help)" ;;
  esac
done

case "$MOTIRIS_PLUGINS" in
  ask|none|context-compaction) ;;
  *) die "unknown plugin selection: $MOTIRIS_PLUGINS (use context-compaction or none)" ;;
esac
if [ "$MOTIRIS_PLUGINS" = ask ]; then
  if [ -t 0 ]; then
    printf 'Install optional context-compaction plugin? [y/N] '
    read answer || answer=""
    case "$answer" in y|Y|yes|YES) MOTIRIS_PLUGINS=context-compaction ;; *) MOTIRIS_PLUGINS=none ;; esac
  else
    MOTIRIS_PLUGINS=none
  fi
fi

detect_prefix() {
  [ -n "$MOTIRIS_PREFIX" ] && { echo "$MOTIRIS_PREFIX"; return; }
  case ":$PATH:" in *":$HOME/bin:"*) echo "$HOME/bin" ;; *) echo "${XDG_BIN_HOME:-$HOME/.local/bin}" ;; esac
}
detect_arch() {
  case "$(uname -m)" in x86_64|amd64) echo x86_64 ;; *) die "no prebuilt release for $(uname -m); build from source" ;; esac
}
find_latest_tag() {
  if [ -n "${MOTIRIS_RELEASE_TAG:-}" ]; then printf '%s\n' "$MOTIRIS_RELEASE_TAG"; return; fi
  url="https://api.github.com/repos/$MOTIRIS_REPO/releases/latest"
  if command -v jq >/dev/null 2>&1; then curl -fsSL "$url" | jq -r .tag_name
  else curl -fsSL "$url" | grep -o '"tag_name"[^,]*' | head -1 | cut -d'"' -f4; fi
}
download() { curl -fsSL -o "$2" "$1" || die "download failed: $1"; }
checksum_line_for() {
  awk -v file="$2" '$2 == file || $2 == "*" file { print; found=1; exit } END { exit !found }' "$1"
}
verify_archive_members() {
  archive=$1 kind=$2
  members=$(tar tzf "$archive") || die "cannot list $archive"
  if [ "$kind" = main ]; then
    [ "$members" = motiris ] || die "unexpected main archive members; refusing extraction"
  else
    [ "$members" = "context_compaction/context_compaction.so
context_compaction/SKILL.md" ] || die "unexpected plugin archive members; refusing extraction"
  fi
}

ARCH=$(detect_arch)
TAG=$(find_latest_tag)
[ -n "$TAG" ] || die "cannot resolve the latest release for $MOTIRIS_REPO"
V=${TAG#v}
BASE="${MOTIRIS_DOWNLOAD_BASE:-https://github.com/$MOTIRIS_REPO/releases/download/$TAG}"
TARBALL="motiris-${V}-linux-${ARCH}.tar.gz"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP/downloads" "$TMP/main" "$TMP/plugin"

say "release: $TAG, asset: $TARBALL"
download "$BASE/$TARBALL" "$TMP/downloads/$TARBALL"
download "$BASE/motiris.sha256" "$TMP/downloads/motiris.sha256"
# New releases name the tarball and are verified before extraction. Legacy
# releases named only the binary: retain compatibility, but verify that binary
# in an empty extraction directory and reject every unexpected archive member.
if checksum_line_for "$TMP/downloads/motiris.sha256" "$TARBALL" > "$TMP/downloads/main.check"; then
  (cd "$TMP/downloads" && sha256sum -c main.check >/dev/null) || die "archive checksum mismatch"
  say "archive checksum OK (verified before extraction)"
else
  say "legacy checksum format; validating archive before binary verification"
fi
verify_archive_members "$TMP/downloads/$TARBALL" main
tar xzf "$TMP/downloads/$TARBALL" -C "$TMP/main" || die "cannot unpack $TARBALL"
[ -x "$TMP/main/motiris" ] || die "archive has no executable motiris"
awk '{print $1 "  motiris"; exit}' "$TMP/downloads/motiris.sha256" > "$TMP/main/legacy.check"
if [ ! -s "$TMP/downloads/main.check" ]; then
  (cd "$TMP/main" && sha256sum -c legacy.check >/dev/null) || die "binary checksum mismatch"
fi
say "binary checksum OK"

PREFIX=$(detect_prefix)
mkdir -p "$PREFIX" || die "cannot create $PREFIX"
install -m 0755 "$TMP/main/motiris" "$PREFIX/motiris" || die "install failed"
say "installed: $PREFIX/motiris"
VER=$("$PREFIX/motiris" --version 2>/dev/null || echo "motiris (version unknown)")
say "binary: $VER"

CFG_DIR="${MOTIRIS_HOME:-$HOME/.motiris}"
# noclobber plus the existence checks keeps user files and symlinks untouched.
new_file() {
  path=$1
  if [ -e "$path" ] || [ -L "$path" ]; then return 1; fi
  (set -C; umask 077; cat > "$path")
}
write_default() {
  if new_file "$1"; then say "written: $1"; else say "exists, kept as-is: $1"; fi
}

if [ "$MOTIRIS_SKIP_CONFIG" = 1 ]; then
  say "config and default documents skipped (--skip-config)"
else
  mkdir -p "$CFG_DIR/skills/motiris-config" "$CFG_DIR/skills/motiris-session" \
    "$CFG_DIR/skills/motiris-context-compaction"
  if [ -f "$CFG_DIR/config.json" ]; then say "config exists, kept as-is: $CFG_DIR/config.json"
  else
    write_default "$CFG_DIR/config.json" <<CFG
{
  "model": "$MOTIRIS_MODEL",
  "base_url": "$MOTIRIS_BASE_URL",
  "api_key_env": "MOTIRIS_API_KEY",
  "max_steps": 0,
  "transport": "auto",
  "stream": true,
  "approval": "off",
  "workspace_guard": false,
  "reasoning_effort": "medium",
  "show_thinking": false,
  "context_compaction": false,
  "compaction_threshold": 32,
  "compaction_keep": 8,
  "compaction_max_chars": 8192,
  "tools": true,
  "plugins": true,
  "system": "You are iris, a concise helpful assistant.",
  "shell_deny": "shutdown,reboot,poweroff,halt,mkfs,dd if"
}
CFG
  fi
  if [ ! -f "$CFG_DIR/env" ]; then
    KEY=""
    if [ -n "${MOTIRIS_API_KEY:-}" ]; then KEY="$MOTIRIS_API_KEY"
    elif [ -n "${MOI_TAAS_API_KEY:-}" ]; then KEY="$MOI_TAAS_API_KEY"; fi
    if [ -n "$KEY" ]; then printf 'MOTIRIS_API_KEY=%s\n' "$KEY" | write_default "$CFG_DIR/env"; chmod 600 "$CFG_DIR/env"
    else write_default "$CFG_DIR/env" <<ENV
MOTIRIS_API_KEY=\${$MOTIRIS_API_KEY_ENV:-}
MOTIRIS_MODEL=$MOTIRIS_MODEL
MOTIRIS_BASE_URL=$MOTIRIS_BASE_URL
MOTIRIS_PLUGIN_DIR=$CFG_DIR/plugins
ENV
    fi
  else say "env exists, kept as-is: $CFG_DIR/env"; fi

  write_default "$CFG_DIR/AGENTS.md" <<'EOF'
# AGENTS.md

- Inspect before changing; keep changes bounded and verify the result.
- Session, read/write, MCP and approval are built in; do not install duplicate plugins.
- approval and workspace_guard default off; file tools may access any OS-permitted path.
- Whenever a feature changes, update its relevant default installed SKILL.md too.
- Never overwrite existing configuration or credentials during initialization.
EOF
  write_default "$CFG_DIR/SOUL.md" <<'EOF'
# SOUL.md

You are iris: direct, helpful and concise. State uncertainty plainly, prefer
verifiable facts, and respect the user's workspace and choices.
EOF
  write_default "$CFG_DIR/skills/motiris-config/SKILL.md" <<'EOF'
---
name: motiris-config
description: Configure runtime defaults, reasoning, approval and directory-driven skills.
---
# Motiris configuration
Use `motiris --config KEY VALUE` or `/config`, `/approval`, `/reasoning`, `/thinking`.
`max_steps=0` is unlimited; SSE is on, while approval and workspace_guard are off.
With workspace_guard off, file tools access any OS-permitted path; enable it for
a directory boundary. Skill loading scans user < workspace < explicit directories, indexes only metadata,
and loads an indexed body on demand with `read_file`. Optional compaction uses
`context_compaction` and its three bounded numeric settings. Never print secrets
or overwrite existing configuration unless explicitly requested.
EOF
  write_default "$CFG_DIR/skills/motiris-session/SKILL.md" <<'EOF'
---
name: motiris-session
description: Save, restore and reset built-in automatic UUID sessions.
---
# Motiris sessions
The index is `session/session.json`; traces are `session/session-<uuid>.json`
under MOTIRIS_HOME. REPL turns save automatically. Use `/save`, `/load` and
`/new`; Gateway restores by chat_id. Reasoning side-channel text is not traced.
EOF
  write_default "$CFG_DIR/skills/motiris-context-compaction/SKILL.md" <<'EOF'
---
name: motiris-context-compaction
description: Configure the optional deterministic bounded compaction plugin.
---
# Context compaction
Keep `context_compaction` false unless the optional plugin is installed. Tune
`compaction_threshold`, `compaction_keep`, and `compaction_max_chars`. The plugin
preserves the active user turn and complete tool pairs; failures retain context.
EOF
fi

if [ "$MOTIRIS_PLUGINS" = context-compaction ]; then
  PLUGIN_TAR="motiris-context-compaction-${V}-linux-${ARCH}.tar.gz"
  download "$BASE/$PLUGIN_TAR" "$TMP/downloads/$PLUGIN_TAR"
  download "$BASE/motiris-context-compaction.sha256" "$TMP/downloads/motiris-context-compaction.sha256"
  checksum_line_for "$TMP/downloads/motiris-context-compaction.sha256" "$PLUGIN_TAR" > "$TMP/downloads/plugin.check" \
    || die "plugin checksum does not name its archive"
  (cd "$TMP/downloads" && sha256sum -c plugin.check >/dev/null) || die "plugin archive checksum mismatch"
  verify_archive_members "$TMP/downloads/$PLUGIN_TAR" plugin
  tar xzf "$TMP/downloads/$PLUGIN_TAR" -C "$TMP/plugin" || die "cannot unpack plugin"
  mkdir -p "$CFG_DIR/plugins/context_compaction" "$CFG_DIR/skills/motiris-context-compaction"
  install -m 0755 "$TMP/plugin/context_compaction/context_compaction.so" \
    "$CFG_DIR/plugins/context_compaction/context_compaction.so"
  if [ ! -e "$CFG_DIR/skills/motiris-context-compaction/SKILL.md" ]; then
    install -m 0644 "$TMP/plugin/context_compaction/SKILL.md" \
      "$CFG_DIR/skills/motiris-context-compaction/SKILL.md"
  fi
  say "optional context-compaction plugin installed and archive verified"
  if [ "$MOTIRIS_SKIP_CONFIG" != 1 ]; then
    MOTIRIS_HOME="$CFG_DIR" "$PREFIX/motiris" --config context_compaction true \
      || die "plugin installed but could not enable context_compaction"
    say "context_compaction enabled; existing scalar settings preserved"
  fi
else
  say "optional plugins: none (all session/read/write/MCP/approval features are built in)"
fi

case ":$PATH:" in *":$PREFIX:"*) ;; *) say "note: add $PREFIX to PATH" ;; esac
say "done. try: motiris -p \"hello\"  or  motiris  (interactive)"
