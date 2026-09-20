#!/bin/sh
# motiris install.sh — fetch the latest release from GitHub, verify the
# checksum, install the binary, and generate a working config
# (model/base_url/api key/tool defaults). Pure POSIX sh; jq optional.
set -eu

MOTIRIS_REPO="${MOTIRIS_REPO:-Shen-KD/motiris}"
MOTIRIS_BASE_URL="${MOTIRIS_BASE_URL:-https://token.moi.matrixorigin.cn/v1/chat/completions}"
MOTIRIS_MODEL="${MOTIRIS_MODEL:-deepseek-v4-flash}"
MOTIRIS_API_KEY_ENV="${MOTIRIS_API_KEY_ENV:-MOI_TAAS_API_KEY}"
MOTIRIS_SKIP_CONFIG="${MOTIRIS_SKIP_CONFIG:-0}"
MOTIRIS_PREFIX="${MOTIRIS_PREFIX:-}"

say() { printf '\033[1;36mmotiris\033[0m %s\n' "$*"; }
die() { printf '\033[31mmotiris install: %s\033[0m\n' "$*" >&2; exit 1; }

usage() {
  cat <<'EOF'
usage: install.sh [--prefix DIR] [--skip-config] [--base-url URL]
                  [--model NAME] [--api-key-env VAR]

Downloads the latest motiris release from GitHub, installs the binary
(prefix: --prefix, else $HOME/bin if on PATH, else $HOME/.local/bin),
then writes ~/.motiris/{env,config.json} unless they already exist.

Env knobs: MOTIRIS_REPO, MOTIRIS_PREFIX, MOTIRIS_BASE_URL,
MOTIRIS_MODEL, MOTIRIS_API_KEY_ENV, MOTIRIS_SKIP_CONFIG.
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
    *) die "unknown argument: $arg (see --help)" ;;
  esac
done

detect_prefix() {
  [ -n "$MOTIRIS_PREFIX" ] && { echo "$MOTIRIS_PREFIX"; return; }
  case ":$PATH:" in
    *":$HOME/bin:"*) echo "$HOME/bin" ;;
    *) echo "${XDG_BIN_HOME:-$HOME/.local/bin}" ;;
  esac
}

detect_arch() {
  case "$(uname -m)" in
    x86_64|amd64) echo x86_64 ;;
    *) die "no prebuilt release for $(uname -m); build from source (make)" ;;
  esac
}

# latest tag name from the GitHub API (jq if present, else grep/sed)
find_latest_tag() {
  url="https://api.github.com/repos/$MOTIRIS_REPO/releases/latest"
  if command -v jq >/dev/null 2>&1; then
    curl -fsSL "$url" | jq -r .tag_name
  else
    curl -fsSL "$url" | grep -o '"tag_name"[^,]*' | head -1 |
      cut -d'"' -f4
  fi
}
# ---------------- download & verify ----------------
ARCH=$(detect_arch)
TAG=$(find_latest_tag)
[ -n "$TAG" ] || die "cannot resolve the latest release for $MOTIRIS_REPO"
V=${TAG#v}

BASE="https://github.com/$MOTIRIS_REPO/releases/download/$TAG"
TARBALL="motiris-${V}-linux-${ARCH}.tar.gz"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

say "release: $TAG, asset: $TARBALL"
curl -fsSL -o "$TMP/$TARBALL" "$BASE/$TARBALL" \
  || die "download failed: $BASE/$TARBALL"
curl -fsSL -o "$TMP/motiris.sha256" "$BASE/motiris.sha256" \
  || die "checksum asset missing: $BASE/motiris.sha256"

cd "$TMP"
tar xzf "$TARBALL" || die "cannot unpack $TARBALL"
awk '{print $1 "  motiris"}' motiris.sha256 > motiris.check
sha256sum -c motiris.check >/dev/null 2>&1 \
  || die "checksum mismatch — aborted (asset $TARBALL corrupted or tampered)"
[ -x motiris ] || die "archive has no executable 'motiris'"
say "checksum OK"

# ---------------- install ----------------
PREFIX=$(detect_prefix)
mkdir -p "$PREFIX" || die "cannot create $PREFIX"
install -m 0755 motiris "$PREFIX/motiris" || die "install failed"
say "installed: $PREFIX/motiris"

VER=$("$PREFIX/motiris" --version 2>/dev/null || echo "motiris (version unknown)")
say "binary: $VER"

# ---------------- config bootstrap ----------------
if [ "$MOTIRIS_SKIP_CONFIG" = "1" ]; then
  say "config skipped (--skip-config)"
else
  CFG_DIR="${MOTIRIS_HOME:-$HOME/.motiris}"
  if [ -f "$CFG_DIR/config.json" ]; then
    say "config exists: $CFG_DIR/config.json (kept as-is)"
  else
    mkdir -p "$CFG_DIR"
    cat > "$CFG_DIR/config.json" <<CFG
{
  "model": "$MOTIRIS_MODEL",
  "base_url": "$MOTIRIS_BASE_URL",
  "api_key_env": "MOTIRIS_API_KEY",
  "max_steps": 10,
  "transport": "auto",
  "stream": true,
  "tools": true,
  "plugins": true,
  "system": "You are iris, a concise helpful assistant.",
  "shell_deny": "shutdown,reboot,poweroff,halt,mkfs,dd if"
}
CFG
    say "config written: $CFG_DIR/config.json"
  fi

  ENV_FILE="$CFG_DIR/env"
  if [ -f "$ENV_FILE" ]; then
    say "env file exists: $ENV_FILE (kept as-is)"
  else
    KEY=""
    if [ -n "${MOTIRIS_API_KEY:-}" ]; then
      KEY="$MOTIRIS_API_KEY"
    elif [ -n "${MOI_TAAS_API_KEY:-}" ]; then
      KEY="$MOI_TAAS_API_KEY"
    fi
    if [ -n "$KEY" ]; then
      printf 'MOTIRIS_API_KEY=%s\n' "$KEY" > "$ENV_FILE"
      chmod 600 "$ENV_FILE"
      say "env written with API key (mode 600)"
    else
      printf 'MOTIRIS_API_KEY=${%s:-}\n' "$MOTIRIS_API_KEY_ENV" > "$ENV_FILE"
      say "env written; export $MOTIRIS_API_KEY_ENV in your shell, then re-run"
    fi
  fi
fi

# ---------------- path hint ----------------
case ":$PATH:" in
  *":$PREFIX:"*) ;;
  *) say "note: $PREFIX is not on PATH — add: export PATH=\"$PREFIX:\$PATH\"" ;;
esac

say "done. try: motiris -p \"hello\"  or  motiris  (interactive)"
