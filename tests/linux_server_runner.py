"""Own a Linux test server until stdin closes or the parent requests shutdown."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--server", required=True)
    parser.add_argument("--config", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--webrtc-log", choices=["error", "info"], default="error")
    args = parser.parse_args()
    root = Path(args.output)
    with (root / "linux-server.stdout.log").open("w") as out, (root / "linux-server.stderr.log").open("w") as err:
        env = os.environ.copy()
        env["LK_WEBRTC_LOG"] = args.webrtc_log
        child = subprocess.Popen([args.server, "--config", args.config], stdout=out, stderr=err, env=env)
        try:
            (root / "linux-process.json").write_text(json.dumps({"pid":child.pid, "server":args.server}), encoding="utf-8")
            sys.stdin.readline()
        finally:
            if child.poll() is None:
                child.terminate()
                try:
                    child.wait(timeout=15)
                except subprocess.TimeoutExpired:
                    child.kill()
                    child.wait(timeout=5)
            (root / "linux-exit.json").write_text(json.dumps({"pid":child.pid, "exit_code":child.returncode}), encoding="utf-8")
        return child.returncode


if __name__ == "__main__":
    sys.exit(main())
