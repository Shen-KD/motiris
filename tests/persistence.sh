#!/bin/sh
# Verify the full host CLI with the checkpoint file backend compiled out.
set -eu
cd "$(dirname "$0")/.."

test ! -e src/harness/checkpoints.o
test -f src/harness/checkpoints_nofs.o
if nm -D motiris | grep -E '[[:space:]](fsync|rename)(@|$)'; then
  echo "FAIL persistence cut still imports fsync/rename" >&2
  exit 1
fi
if strings motiris | grep -q '\.motiris-checkpoint-'; then
  echo "FAIL persistence cut retains the atomic-file backend" >&2
  exit 1
fi
python3 tests/persistence.py
