"""Linux-only coturn UDP/TCP acceptance with relay-only browser media and owned processes."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import secrets
import signal
import shutil
import subprocess
import sys
import time
from sdk_cli_integration import free_port, wait_server
from udp_impairment import UdpImpairment


def executable(value):
    resolved = shutil.which(value)
    if not resolved:
        raise ValueError(f"Executable not found: {value}")
    return str(Path(resolved).resolve())


def stop(child):
    forced = False
    if child.poll() is None:
        child.terminate()
        try:
            child.wait(timeout=15)
        except subprocess.TimeoutExpired:
            forced = True
            child.kill()
            child.wait(timeout=5)
    return {"pid":child.pid, "exit_code":child.returncode, "forced":forced}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server", required=True)
    parser.add_argument("--turnserver", default="turnserver")
    parser.add_argument("--node", default="node")
    parser.add_argument("--chrome", default=os.environ.get("LK_CHROME", "google-chrome"))
    parser.add_argument("--output", default="tests/results/turn")
    parser.add_argument("--outage", action="store_true", help="Drop both directions of TURN/UDP for five seconds and assert media recovery")
    parser.add_argument("--rest-auth", action="store_true", help="Use server-issued expiring coturn REST credentials")
    args = parser.parse_args()
    if not sys.platform.startswith("linux"):
        parser.error("Run this driver in Linux; it owns local Linux coturn/server/browser processes")
    binaries = {key:executable(getattr(args, key)) for key in ("server", "turnserver", "node", "chrome")}
    root = Path(args.output).resolve() / time.strftime("%Y%m%d-%H%M%S")
    root.mkdir(parents=True)
    project = Path(__file__).resolve().parent.parent
    metadata = {"platform":sys.platform, "turn_auth":"rest" if args.rest_auth else "static", "binaries":{key:{"path":value,
        "sha256":hashlib.sha256(Path(value).read_bytes()).hexdigest()} for key,value in binaries.items()}}
    (root / "environment.json").write_text(json.dumps(metadata, indent=2), encoding="utf-8")
    results = []
    for transport in ("udp", "tcp"):
        directory = root / transport
        directory.mkdir()
        port, turn_port = free_port(), free_port()
        while turn_port == port:
            turn_port = free_port()
        username, password = "test-" + secrets.token_hex(8), secrets.token_urlsafe(32)
        impairment = UdpImpairment(("127.0.0.1", turn_port)) if args.outage and transport=="udp" else None
        client_port = impairment.port if impairment else turn_port
        config = directory / "server.json"
        urls = [f"turn:127.0.0.1:{client_port}?transport={transport}"]
        rtc = {"port_range_start":54000, "port_range_end":54499}
        server_env = os.environ.copy()
        if args.rest_auth:
            rtc["turn_rest"] = {"urls":urls, "shared_secret_env":"LK_TEST_COTURN_SECRET", "credential_ttl_seconds":3600}
            server_env["LK_TEST_COTURN_SECRET"] = password
        else:
            rtc["ice_servers"] = [{"urls":urls, "username":username, "credential":password}]
        config.write_text(json.dumps({"bind":"127.0.0.1", "port":port,
            "keys":{"devkey":"secret"}, "rtc":rtc}, indent=2), encoding="utf-8")
        config.chmod(0o600)
        turn_config = directory / "turnserver.conf"
        # Loopback permissions are restricted to this isolated local test. Do
        # not use this configuration as an Internet-facing deployment template.
        turn_config.write_text("\n".join([
            "listening-ip=127.0.0.1", "relay-ip=127.0.0.1", f"listening-port={turn_port}",
            "min-port=54500", "max-port=54999", "realm=livekit-test.local",
            "use-auth-secret" if args.rest_auth else "lt-cred-mech",
            f"static-auth-secret={password}" if args.rest_auth else f"user={username}:{password}",
            "fingerprint", "no-cli", "no-tls", "no-dtls",
            "allow-loopback-peers", "no-multicast-peers", "no-tcp" if transport=="udp" else "no-udp",
            "relay-threads=1", "user-quota=32", "total-quota=64", "simple-log", "log-file=stdout",
            f"pidfile={directory / 'turn.pid'}", ""]) , encoding="utf-8")
        turn_config.chmod(0o600)
        suite = {"transport":transport, "ok":False}
        with (directory / "turn.log").open("w") as turn_log, (directory / "server.log").open("w") as server_log, \
             (directory / "browser.log").open("w") as browser_log:
            turn = subprocess.Popen([binaries["turnserver"], "-c", str(turn_config)], stdout=turn_log, stderr=turn_log)
            server = None
            try:
                server = subprocess.Popen([binaries["server"], "--config", str(config)], stdout=server_log, stderr=server_log, env=server_env)
                url = f"http://127.0.0.1:{port}"
                wait_server(url, server)
                if turn.poll() is not None:
                    raise RuntimeError("coturn exited before media validation")
                env = os.environ.copy()
                env.update(LK_EXTERNAL_SERVER="1", LK_TEST_URL=url, LK_FORCE_RELAY="1",
                           LK_TURN_TRANSPORT=transport, LK_CHROME=binaries["chrome"],
                           LK_TEST_RESULTS=str(directory / "browser"))
                env.pop("LK_IMPAIRMENT_URL", None)
                env.pop("LK_IMPAIRMENT_TOKEN", None)
                env.pop("LK_EXPECT_TURN_REST", None)
                env.pop("LK_TEST_COTURN_SECRET", None)
                if args.rest_auth:
                    env.update(LK_EXPECT_TURN_REST="1", LK_TEST_COTURN_SECRET=password)
                if impairment:
                    env.update(LK_IMPAIRMENT_URL=impairment.url, LK_IMPAIRMENT_TOKEN=impairment.token)
                browser = subprocess.Popen([binaries["node"], "tests/integration.mjs"], cwd=project,
                    env=env, stdout=browser_log, stderr=browser_log, start_new_session=True)
                try:
                    browser.wait(timeout=240)
                except subprocess.TimeoutExpired:
                    # Kill the owned process group, including Chromium children.
                    os.killpg(browser.pid, signal.SIGKILL)
                    browser.wait(timeout=5)
                    raise
                suite["browser_exit_code"] = browser.returncode
                suite["turn_alive_after_media"] = turn.poll() is None
                suite["ok"] = browser.returncode == 0 and suite["turn_alive_after_media"]
            except Exception as error:
                suite["error"] = str(error)
            finally:
                if impairment:
                    suite["impairment"] = impairment.snapshot()
                    try:
                        impairment.close()
                    except Exception as error:
                        suite["cleanup_error"] = str(error)
                if server:
                    suite["server_exit"] = stop(server)
                suite["turn_exit"] = stop(turn)
                suite["ok"] = (suite["ok"] and suite.get("server_exit", {}).get("exit_code") == 0
                               and not suite["turn_exit"]["forced"]
                               and not suite.get("impairment", {}).get("errors") and "cleanup_error" not in suite)
        if args.rest_auth:
            logs = [directory / name for name in ("server.log", "browser.log", "turn.log")]
            logs += list((directory / "browser").glob("*.json")) + list((directory / "browser").glob("*.log"))
            suite["secret_redaction_verified"] = all(password not in path.read_text(encoding="utf-8", errors="replace")
                                                      for path in logs if path.is_file())
            suite["ok"] = suite["ok"] and suite["secret_redaction_verified"]
        results.append(suite)
        (root / "result.json").write_text(json.dumps({"ok":len(results)==2 and all(s["ok"] for s in results),
            "suites":results, **metadata}, indent=2), encoding="utf-8")
        print(f"{'PASS' if suite['ok'] else 'FAIL'} TURN/{transport.upper()}: {directory}", flush=True)
    return 0 if all(s["ok"] for s in results) else 1


if __name__ == "__main__":
    sys.exit(main())
