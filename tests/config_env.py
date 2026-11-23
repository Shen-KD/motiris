#!/usr/bin/env python3
"""Bounded, overlap-safe env-file expansion and precedence regressions (#74)."""

import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
BIN = ROOT / "motiris"


def run_case(root, name, env_text, extra=None, config=None):
    home = root / (name + "-home")
    cfg = root / (name + "-config")
    home.mkdir()
    cfg.mkdir()
    (cfg / "env").write_text(env_text)
    if config is not None:
        (cfg / "config.json").write_text(config)
    env = {key: value for key, value in os.environ.items()
           if not key.startswith("MOTIRIS_")}
    env.update({"HOME": str(home), "MOTIRIS_HOME": str(cfg)})
    if extra:
        env.update(extra)
    proc = subprocess.run(
        [str(BIN), "-p", "hi", "--transport", "echo", "--no-tools",
         "--no-plugin", "-v"], cwd=ROOT, env=env, text=True,
        capture_output=True, timeout=4)
    assert proc.returncode == 0, (name, proc.stdout, proc.stderr)
    return proc.stdout + proc.stderr


def main():
    with tempfile.TemporaryDirectory(prefix="motiris-config-env-") as tmp:
        root = Path(tmp)
        output = run_case(root, "inline", "MOTIRIS_MODEL=${FOO:-abc}TAIL\n")
        assert "abcTAIL via" in output, output

        for name, extra in [
            ("self", {"FOO": "${FOO}"}),
            ("mutual", {"FOO": "${BAR}", "BAR": "${FOO}"}),
        ]:
            output = run_case(root, name, "MOTIRIS_MODEL=${FOO}\n", extra)
            assert "invalid or recursive expansion skipped" in output, output
            assert "gpt-4o-mini via" in output, output

        dummy = "".join("MOTIRIS_DUMMY_%d=value\n" % i for i in range(24))
        output = run_case(root, "provenance",
                          dummy + "MOTIRIS_MODEL=file-model\n",
                          config='{"model":"config-model"}')
        assert "config-model via" in output, output
        assert "file-model via" not in output, output

        output = run_case(root, "shell", "MOTIRIS_MODEL=file-model\n",
                          {"MOTIRIS_MODEL": "shell-model"},
                          config='{"model":"config-model"}')
        assert "shell-model via" in output, output

        output = run_case(root, "long", "MOTIRIS_MODEL=${FOO:-abc}TAIL\n",
                          {"FOO": "x" * 5000})
        assert "invalid or recursive expansion skipped" in output, output
        assert "x" * 100 not in output, output
    print("config-env: overlap, recursion, long values and >16-key precedence passed")


if __name__ == "__main__":
    main()
