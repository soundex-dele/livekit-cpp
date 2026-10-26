"""Adapted from livekit-sdk-cpp/tests/integration.py for this C++ server.
Runs against a private local C++ LiveKit server; original CLI assertions retained. Requires Python 3 and FFmpeg.
All children are owned by this test; logs and recordings are retained in --output.
"""
import argparse
import base64
import hashlib
import hmac
import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import time
import wave
import threading
import urllib.request


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def wait_server(url, process=None, timeout=20):
    deadline = time.monotonic() + timeout
    while True:
        if process is not None and process.poll() is not None:
            raise AssertionError("Server exited before becoming healthy")
        try:
            with urllib.request.urlopen(url.rstrip('/') + '/healthz', timeout=1) as response:
                if response.status == 200:
                    return
        except OSError:
            pass
        if time.monotonic() >= deadline:
            raise AssertionError(f"Server never became healthy: {url}")
        time.sleep(.1)


def record_metrics(root, port, secret, stop, samples, lock, recording_paths=(), base_url=None):
    """Authenticated transport samples and optional decoded-recording growth."""
    b64 = lambda b: base64.urlsafe_b64encode(b).rstrip(b"=").decode()
    head = b64(b'{"alg":"HS256","typ":"JWT"}')
    body = b64(json.dumps({"iss":"testkey", "exp":int(time.time())+600, "video":{"roomList":True}}).encode())
    signature = b64(hmac.new(secret.encode(), f"{head}.{body}".encode(), hashlib.sha256).digest())
    url = (base_url or f"http://127.0.0.1:{port}").rstrip('/')
    request = urllib.request.Request(f"{url}/metrics", headers={"Authorization":f"Bearer {head}.{body}.{signature}"})
    with (root / "metrics.jsonl").open("w", encoding="utf-8") as log:
        while not stop.is_set():
            sample = {"time":time.time()}
            try:
                with urllib.request.urlopen(request, timeout=1) as response:
                    for line in response.read().decode().splitlines():
                        key, value = line.split()
                        sample[key.removeprefix("livekit_cpp_")] = float(value)
                if recording_paths:
                    sample["recording_bytes"] = {str(p.relative_to(root)):p.stat().st_size
                        for directory in recording_paths for p in directory.glob("*.y4m")}
            except Exception as error:
                sample["error"] = type(error).__name__
            with lock:
                samples.append(sample)
            log.write(json.dumps(sample) + "\n"); log.flush()
            stop.wait(.2)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--lk", required=True)
    parser.add_argument("--server", required=True)
    parser.add_argument("--external-url", help="Use an already running dedicated test server; --server identifies its binary for evidence")
    parser.add_argument("--ffmpeg", default="ffmpeg")
    parser.add_argument("--output", default="tests/results/sdk-cli-integration")
    args = parser.parse_args()
    root = Path(args.output).resolve() / time.strftime("%Y%m%d-%H%M%S")
    root.mkdir(parents=True)
    lk = str(Path(args.lk).resolve())
    port = free_port()
    env = os.environ.copy()
    server_url = args.external_url or f"http://127.0.0.1:{port}"
    env.update(LIVEKIT_URL=server_url, LIVEKIT_API_KEY="testkey",
               LIVEKIT_API_SECRET="integration-test-secret-32-characters", LIVEKIT_TOKEN="")
    children = []
    handles = []
    creationflags = subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0
    metrics_stop = threading.Event()
    metrics_thread = None
    metrics_samples = []
    metrics_lock = threading.Lock()

    def run(*cmd, ok=True, timeout=45):
        result = subprocess.run([lk, *cmd], env=env, capture_output=True, text=True,
                                encoding="utf-8", errors="replace", timeout=timeout, creationflags=creationflags)
        if ok and result.returncode:
            raise AssertionError(f"{cmd}: {result.returncode}\n{result.stdout}\n{result.stderr}")
        if not ok and result.returncode == 0:
            raise AssertionError(f"Expected failure: {cmd}")
        return result.stdout

    def start(label, cmd, child_env=env):
        out = open(root / f"{label}.stdout.log", "w", encoding="utf-8")
        err = open(root / f"{label}.stderr.log", "w", encoding="utf-8")
        handles.extend([out, err])
        process = subprocess.Popen(cmd, env=child_env, stdout=out, stderr=err, creationflags=creationflags)
        children.append(process)
        return process

    def events(label):
        output = (root / f"{label}.stdout.log").read_text(encoding="utf-8", errors="replace")
        return [json.loads(line) for line in output.splitlines() if line.startswith("{")]

    def wait_event(label, process, event, timeout=35):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if any(item.get("event") == event for item in events(label)):
                return
            if process.poll() is not None:
                raise AssertionError(f"{label} exited {process.returncode} before {event}; see {root}")
            time.sleep(.1)
        raise AssertionError(f"{label} never emitted {event}; see {root}")

    def check_recording(directory):
        videos = list(directory.glob("*.y4m"))
        audios = list(directory.glob("*.wav"))
        assert videos and audios, f"Missing audio/video recordings in {directory}"
        assert max(p.stat().st_size for p in videos) > 640 * 360 * 3 // 2 * 20
        audible = False
        for path in audios:
            with wave.open(str(path), "rb") as wav:
                assert wav.getsampwidth() == 2
                pcm = wav.readframes(wav.getnframes())
                if pcm:
                    samples = struct.unpack(f"<{len(pcm)//2}h", pcm)
                    audible |= len(samples) > 48000 and max(abs(s) for s in samples) > 100
        assert audible, "WAV must contain decoded non-silent audio, not only a header"

    try:
        config = {"bind": "127.0.0.1", "port": port,
                  "keys": {"testkey": env["LIVEKIT_API_SECRET"]},
                  "rtc": {"port_range_start": 51000, "port_range_end": 51100}}
        config_path = root / "server.json"
        config_path.write_text(json.dumps(config, indent=2), encoding="utf-8")
        server = None if args.external_url else start("server", [args.server, "--config", str(config_path)])
        wait_server(server_url, server)
        metrics_thread = threading.Thread(target=record_metrics, args=(root,port,env["LIVEKIT_API_SECRET"],metrics_stop,metrics_samples,metrics_lock), kwargs={"base_url":server_url}, daemon=True)
        metrics_thread.start()
        run("doctor")
        token = run("token", "create", "--room", "test", "--identity", "token-test").strip()
        header, claims, signature = token.split(".")
        unb64 = lambda value: base64.urlsafe_b64decode(value + "=" * (-len(value) % 4))
        assert hmac.compare_digest(unb64(signature), hmac.new(env["LIVEKIT_API_SECRET"].encode(), f"{header}.{claims}".encode(), hashlib.sha256).digest())
        assert json.loads(unb64(claims))["video"]["room"] == "test"
        run("room", "create", "test", "--metadata", "initial")
        assert any(room["name"] == "test" for room in json.loads(run("room", "list"))["rooms"])
        run("room", "update", "test", "--metadata", "updated")
        assert json.loads(run("room", "list", "test"))["rooms"][0]["metadata"] == "updated"
        run("--token", "invalid", "room", "join", "test", "--duration", "1", ok=False)
        run("room", "join", "test", "--identity", "invalid-size", "--width", "3", ok=False)
        recording = root / "demo-recording"
        sub = start("demo-sub", [lk, "room", "subscribe", "test", "--identity", "subscriber", "--record", str(recording), "--duration", "12"])
        wait_event("demo-sub", sub, "connected")
        pub = start("demo-pub", [lk, "room", "publish", "test", "--identity", "publisher", "--publish-demo", "--publish-tone", "--publish-data", "hello native", "--topic", "test", "--duration", "6"])
        wait_event("demo-pub", pub, "connected")
        people = json.loads(run("room", "participants", "test"))["participants"]
        assert {p["identity"] for p in people} >= {"publisher", "subscriber"}
        assert pub.wait(timeout=25) == 0
        assert sub.wait(timeout=25) == 0
        check_recording(recording)
        assert any(e.get("event") == "data" for e in events("demo-sub")), "Reliable data not received"
        print("PASS: JWT, room API, native video/Opus, data channel, decoded recording", flush=True)
        fixture = root / "media sample 测试.mkv"
        subprocess.run([args.ffmpeg, "-nostdin", "-v", "error", "-f", "lavfi", "-i", "testsrc2=size=320x180:rate=25", "-f", "lavfi", "-i", "sine=frequency=660:sample_rate=48000", "-t", "4", "-c:v", "libvpx", "-c:a", "libopus", str(fixture)], check=True, timeout=30, creationflags=creationflags)
        file_recording = root / "file-recording"
        sub = start("file-sub", [lk, "room", "subscribe", "test", "--identity", "file-subscriber", "--record", str(file_recording), "--duration", "10"])
        wait_event("file-sub", sub, "connected")
        # Repeating the flag exercises independent tracks and simultaneous child
        # pipes, and catches CLI parsers that accidentally limit the total to one.
        pub = start("file-pub", [lk, "room", "publish", "test", "--identity", "file-publisher", "--publish", str(fixture), "--publish", str(fixture), "--ffmpeg", args.ffmpeg, "--exit-after-publish", "--stats-interval-ms", "500"])
        assert pub.wait(timeout=35) == 0, "File publish failed"
        assert sub.wait(timeout=20) == 0
        check_recording(file_recording)
        received = [e for e in events("file-sub") if e.get("event") == "track-summary"]
        assert len(received) == 4 and all(e["frames"] > 20 for e in received), received
        # Local normal-network regression: each source publishes 120 video
        # frames. A stalled initial bandwidth probe previously passed the basic
        # recording check while losing most frames to the native queue timeout.
        assert all(e["frames"] >= 80 for e in received if e["kind"] == "video"), received
        with metrics_lock:
            samples = [s for s in metrics_samples if "sfu_target_bps" in s]
        assert samples, "No SFU diagnostics samples"
        peaks = {key: max(s.get(key, 0) for s in samples) for key in (
            "sfu_pacer_queue_ms", "sfu_pacer_queue_bytes", "sfu_target_bps",
            "sfu_probe_clusters", "sfu_probe_packets", "sfu_probe_feedback_packets")}
        assert peaks["sfu_pacer_queue_ms"] < 500, peaks
        assert peaks["sfu_probe_feedback_packets"] > 0, peaks
        assert all(s["video_encode_calls"] == 0 and s["video_decode_calls"] == 0 for s in samples)
        (root / "media-evidence.json").write_text(json.dumps({
            "server_sha256": hashlib.sha256(Path(args.server).read_bytes()).hexdigest(),
            "sdk_sha256": hashlib.sha256(Path(lk).read_bytes()).hexdigest(),
            "server_url":server_url, "external_server":bool(args.external_url),
            "file_tracks": received, "sampled_peaks": peaks,
        }, indent=2), encoding="utf-8")
        print("PASS: FFmpeg file decode, Unicode paths, repeated publish (4 tracks), EOF shutdown", flush=True)
        run("room", "delete", "test")
        assert not json.loads(run("room", "list", "test")).get("rooms", [])
        (root / "result.json").write_text(json.dumps({"ok": True, "server": args.server, "checks": ["token", "room-api", "negative-inputs", "demo-video", "opus-audio", "data-channel", "file-publish", "unicode-path", "recording", "shutdown"]}, indent=2), encoding="utf-8")
        print(f"PASS: integration; artifacts: {root}", flush=True)
    finally:
        metrics_stop.set()
        if metrics_thread:
            metrics_thread.join(timeout=2)
        for child in reversed(children):
            if child.poll() is None:
                child.terminate()
                try:
                    child.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    child.kill()
                    child.wait(timeout=10)
        for handle in handles:
            handle.close()


if __name__ == "__main__":
    main()
