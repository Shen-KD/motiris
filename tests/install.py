#!/usr/bin/env python3
"""Local release-installer tests; no GitHub or external provider access."""
import hashlib
import http.server
import json
import os
from pathlib import Path
import subprocess
import tarfile
import tempfile
import threading

ROOT = Path(__file__).resolve().parents[1]


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def make_assets(directory):
    directory.mkdir(parents=True)
    version = "9.9.9"
    main = directory / ("motiris-%s-linux-x86_64.tar.gz" % version)
    with tarfile.open(main, "w:gz") as archive:
        archive.add(ROOT / "motiris", arcname="motiris")
    (directory / "motiris.sha256").write_text("%s  %s\n" % (digest(main), main.name))
    plugin = directory / ("motiris-context-compaction-%s-linux-x86_64.tar.gz" % version)
    with tarfile.open(plugin, "w:gz") as archive:
        archive.add(ROOT / "plugins/context_compaction/context_compaction.so",
                    arcname="context_compaction/context_compaction.so")
        archive.add(ROOT / "plugins/context_compaction/SKILL.md",
                    arcname="context_compaction/SKILL.md")
    (directory / "motiris-context-compaction.sha256").write_text(
        "%s  %s\n" % (digest(plugin), plugin.name))
    return main, plugin


class Quiet(http.server.SimpleHTTPRequestHandler):
    def log_message(self, *_args):
        pass


def install(asset_url, root, *args):
    home = root / "home"
    home.mkdir(parents=True, exist_ok=True)
    env = {key: value for key, value in os.environ.items()
           if not key.startswith("MOTIRIS_")}
    env.update(HOME=str(home), MOTIRIS_HOME=str(root / "config"),
               MOTIRIS_DOWNLOAD_BASE=asset_url, MOTIRIS_RELEASE_TAG="v9.9.9")
    return subprocess.run(["sh", str(ROOT / "install.sh"), "--prefix=" + str(root / "bin"),
                           *args], env=env, text=True, capture_output=True, timeout=30)


def main():
    with tempfile.TemporaryDirectory(prefix="motiris-install-") as temp:
        root = Path(temp)
        make_assets(root / "assets")
        handler = lambda *args, **kwargs: Quiet(*args, directory=str(root / "assets"), **kwargs)
        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), handler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        url = "http://127.0.0.1:%d" % server.server_address[1]
        try:
            default = root / "default"
            result = install(url, default)
            assert result.returncode == 0, result.stdout + result.stderr
            config = json.loads((default / "config/config.json").read_text())
            assert config["max_steps"] == 0 and config["stream"] is True, config
            assert config["approval"] == "off" and config["context_compaction"] is False, config
            assert config["workspace_guard"] is False, config
            assert (default / "bin/motiris").is_file()
            assert not (default / "config/plugins/context_compaction/context_compaction.so").exists()
            for name in ("motiris-config", "motiris-session", "motiris-context-compaction"):
                assert (default / ("config/skills/%s/SKILL.md" % name)).is_file(), name
            assert "verified before extraction" in result.stdout, result.stdout

            selected = root / "selected"
            (selected / "config").mkdir(parents=True)
            (selected / "config/config.json").write_text(json.dumps({
                "transport": "echo", "max_steps": 7, "custom": "preserve"}))
            (selected / "config/env").write_text("ENV_SENTINEL\n")
            (selected / "config/AGENTS.md").write_text("AGENT_SENTINEL\n")
            result = install(url, selected, "--plugins=context-compaction")
            assert result.returncode == 0, result.stdout + result.stderr
            updated = json.loads((selected / "config/config.json").read_text())
            assert updated["context_compaction"] is True, updated
            assert updated["max_steps"] == 7 and updated["custom"] == "preserve", updated
            assert (selected / "config/env").read_text() == "ENV_SENTINEL\n"
            assert (selected / "config/AGENTS.md").read_text() == "AGENT_SENTINEL\n"
            assert (selected / "config/plugins/context_compaction/context_compaction.so").is_file()

            # A plugin archive that does not match its published hash is never extracted.
            (root / "assets/motiris-context-compaction.sha256").write_text(
                "%s  motiris-context-compaction-9.9.9-linux-x86_64.tar.gz\n" % ("0" * 64))
            tampered = root / "tampered"
            result = install(url, tampered, "--plugins=context-compaction")
            assert result.returncode != 0, result.stdout + result.stderr
            assert "plugin archive checksum mismatch" in result.stderr, result.stderr
            assert not (tampered / "config/plugins/context_compaction/context_compaction.so").exists()
        finally:
            server.shutdown()
            server.server_close()
            thread.join(timeout=5)
    print("install: verified archives, defaults, optional plugin and preservation passed")


if __name__ == "__main__":
    main()
