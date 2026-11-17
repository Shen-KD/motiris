#!/bin/sh
# Verify the full host CLI with the checkpoint file backend compiled out.
set -eu
cd "$(dirname "$0")/.."

test ! -e src/harness/checkpoints.o
test -f src/harness/checkpoints_nofs.o
# The unrelated memory_json backend may legitimately use atomic rename.
# Check the selected checkpoint stub itself rather than banning those symbols
# from the whole binary.
if nm src/harness/checkpoints_nofs.o | grep -E '[[:space:]](fsync|rename)$'; then
  echo "FAIL persistence cut stub imports fsync/rename" >&2
  exit 1
fi
if strings motiris | grep -q '\.motiris-checkpoint-'; then
  echo "FAIL persistence cut retains the atomic-file backend" >&2
  exit 1
fi
python3 tests/persistence.py
