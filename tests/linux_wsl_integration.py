"""Run the unchanged browser/native media acceptance gates against an owned WSL server."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import time
from sdk_cli_integration import free_port, wait_server


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--distro", default="Ubuntu")
    parser.add_argument("--linux-server", required=True)
    parser.add_argument("--lk", required=True)
    parser.add_argument("--ffmpeg", default="ffmpeg")
    parser.add_argument("--output", default="tests/results/linux-wsl")
    parser.add_argument("--webrtc-log", choices=["error", "info"], default="error")
    args = parser.parse_args()
    if os.name != "nt":
        parser.error("This driver runs on Windows with WSL and the Windows native SDK")
    project = Path(__file__).resolve().parent.parent
    root = Path(args.output).resolve() / time.strftime("%Y%m%d-%H%M%S")
    root.mkdir(parents=True)
    flags = subprocess.CREATE_NO_WINDOW

    def wsl(*command):
        result = subprocess.run(["wsl", "-d", args.distro, "--exec", *command], capture_output=True,
                                text=True, encoding="utf-8", errors="replace", timeout=60, creationflags=flags)
        if result.returncode:
            raise RuntimeError(f"WSL command failed: {command}\n{result.stderr}")
        return result.stdout.strip()

    def linux_path(path):
        return wsl("wslpath", "-u", Path(path).resolve().as_posix())

    binary_type = wsl("file", "--brief", "--", args.linux_server)
    assert "ELF 64-bit" in binary_type, binary_type
    server_hash = wsl("sha256sum", "--", args.linux_server).split()[0]
    copied_binary = root / "livekit-server"
    wsl("cp", "--", args.linux_server, linux_path(copied_binary))
    assert hashlib.sha256(copied_binary.read_bytes()).hexdigest() == server_hash
    metadata = {"server_sha256":server_hash, "binary_type":binary_type,
                "linux_server":args.linux_server, "distro":args.distro,
                "kernel":wsl("uname", "-srmo"), "client_platform":sys.platform,
                "os_release":wsl("cat", "/etc/os-release"),
                "shared_libraries":wsl("ldd", args.linux_server),
                "clocksource":wsl("cat", "/sys/devices/system/clocksource/clocksource0/current_clocksource"),
                "sdk_sha256":hashlib.sha256(Path(args.lk).read_bytes()).hexdigest()}
    (root / "environment.json").write_text(json.dumps(metadata, indent=2), encoding="utf-8")
    port = free_port()
    url = f"http://127.0.0.1:{port}"
    config = root / "server.json"
    config.write_text(json.dumps({"bind":"0.0.0.0", "port":port,
        "keys":{"devkey":"secret", "testkey":"integration-test-secret-32-characters"},
        "rtc":{"port_range_start":51400, "port_range_end":51999}}, indent=2), encoding="utf-8")
    checks = []
    failures = []

    def run(label, command, env=None):
        with (root / f"{label}.log").open("w", encoding="utf-8") as log:
            completed = subprocess.run(command, cwd=project, env=env, stdout=log, stderr=subprocess.STDOUT,
                                       timeout=240, creationflags=flags)
        if completed.returncode:
            failures.append({"suite":label, "exit_code":completed.returncode})
            print(f"FAIL Linux server: {label}; see {root / (label + '.log')}", flush=True)
            return
        checks.append(label)
        print(f"PASS Linux server: {label}", flush=True)

    with (root / "runner.log").open("w", encoding="utf-8") as log:
        runner = subprocess.Popen(["wsl", "-d", args.distro, "--exec", "python3",
            linux_path(project / "tests/linux_server_runner.py"), "--server", args.linux_server,
            "--config", linux_path(config), "--output", linux_path(root), "--webrtc-log", args.webrtc_log],
            stdin=subprocess.PIPE, stdout=log, stderr=log, text=True, creationflags=flags)
        try:
            wait_server(url, runner, timeout=30)
            browser_env = os.environ.copy()
            browser_env.update(LK_EXTERNAL_SERVER="1", LK_TEST_URL=url, LK_TEST_RESULTS=str(root / "browser"))
            run("browser", ["node", "tests/integration.mjs"], browser_env)
            common = ["--lk", str(Path(args.lk).resolve()), "--server", str(copied_binary),
                      "--external-url", url, "--ffmpeg", args.ffmpeg]
            run("native-files", [sys.executable, "tests/sdk_cli_integration.py", *common,
                                  "--output", str(root / "native-files")])
            run("native-stream", [sys.executable, "tests/sdk_cli_stream_integration.py", *common,
                                   "--output", str(root / "native-stream")])
        finally:
            # Closing the pipe also requests shutdown if a test raises. The
            # Linux helper terminates only its own Popen child and waits for it.
            runner.communicate(input="stop\n", timeout=25)
            (root / "result.json").write_text(json.dumps({"ok":len(checks)==3 and runner.returncode==0,
                "checks":checks, "failures":failures, "runner_exit_code":runner.returncode, **metadata}, indent=2), encoding="utf-8")
        assert runner.returncode == 0, f"Linux server did not shut down cleanly; see {root}"
        assert not failures, f"Linux interoperability failed: {failures}; see {root}"
    print(f"PASS Linux native server interoperability; artifacts: {root}")


if __name__ == "__main__":
    main()
