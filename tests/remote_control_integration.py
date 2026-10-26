"""C++ SFU regression entry: browser -> native SDK remote-control integration.

No desktop capture or input injection. All processes are owned by the Node runner.
"""
import argparse
import os
from pathlib import Path
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument("--server", required=True, help="C++ media server binary")
parser.add_argument("--output", default="out/remote-control-integration")
args = parser.parse_args()
workspace = Path(__file__).resolve().parents[2]
env = dict(os.environ, RC_MEDIA_SERVER=str(Path(args.server).resolve()), RC_RESULTS=str(Path(args.output).resolve()))
raise SystemExit(subprocess.call(["node", str(workspace / "livekit-app/apps/remote-control/web/tests/integration.mjs")], env=env,
    creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0))
