"""Adapted from livekit-sdk-cpp/tests/stream_integration.py for this C++ server.
Focused TCP H.264, selective subscription and server disconnect tests."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import socket
import subprocess
import time
import threading
from sdk_cli_integration import free_port, record_metrics, wait_server


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--lk", required=True)
    parser.add_argument("--server", required=True)
    parser.add_argument("--external-url", help="Use an already running dedicated test server; --server identifies its binary for evidence")
    parser.add_argument("--output", default="tests/results/sdk-cli-stream")
    parser.add_argument("--ffmpeg", default="ffmpeg")
    args = parser.parse_args()
    root = Path(args.output) / time.strftime("%Y%m%d-%H%M%S")
    root.mkdir(parents=True)
    root = root.resolve()
    lk = str(Path(args.lk).resolve())
    port, source_port = free_port(), free_port()
    env = os.environ.copy()
    server_url = args.external_url or f"http://127.0.0.1:{port}"
    env.update(LIVEKIT_URL=server_url, LIVEKIT_API_KEY="testkey",
               LIVEKIT_API_SECRET="integration-test-secret-32-characters", LIVEKIT_TOKEN="")
    flags = subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0
    children, files = [], []
    metrics_stop = threading.Event()
    metrics_lock = threading.Lock()
    metrics_samples = []
    metrics_thread = None

    def start(label, cmd):
        out = open(root / f"{label}.stdout.log", "w", encoding="utf-8")
        err = open(root / f"{label}.stderr.log", "w", encoding="utf-8")
        files.extend([out, err])
        child = subprocess.Popen(cmd, env=env, stdout=out, stderr=err, creationflags=flags)
        children.append(child)
        return child

    def events(label):
        text = (root / f"{label}.stdout.log").read_text(encoding="utf-8", errors="replace")
        return [json.loads(line) for line in text.splitlines() if line.startswith("{")]

    def wait_event(label, child, name):
        deadline = time.monotonic() + 30
        while time.monotonic() < deadline:
            if any(e.get("event") == name for e in events(label)):
                return
            if child.poll() is not None:
                raise AssertionError(f"{label} exited {child.returncode}; see {root}")
            time.sleep(.1)
        raise AssertionError(f"No {name} from {label}; see {root}")

    def run(*cmd):
        result = subprocess.run([lk, *cmd], env=env, capture_output=True, text=True,
                                encoding="utf-8", errors="replace", timeout=20, creationflags=flags)
        assert result.returncode == 0, result.stderr
        return json.loads(result.stdout)

    try:
        config = root / "server.json"
        config.write_text(json.dumps({"bind": "127.0.0.1", "port": port,
                          "keys": {"testkey": env["LIVEKIT_API_SECRET"]},
                          "rtc": {"port_range_start": 51200, "port_range_end": 51300}},
                         indent=2), encoding="utf-8")
        server = None if args.external_url else start("server", [args.server, "--config", str(config)])
        wait_server(server_url, server)
        metrics_thread = threading.Thread(target=record_metrics, args=(root,port,env["LIVEKIT_API_SECRET"],metrics_stop,metrics_samples,metrics_lock,
            (root / "all-recording",root / "selected-recording")), kwargs={"base_url":server_url}, daemon=True)
        metrics_thread.start()
        sub = start("all", [lk, "room", "subscribe", "tcp-test", "--identity", "all", "--record", str(root / "all-recording"), "--duration", "14", "--stats-interval-ms", "500"])
        wait_event("all", sub, "connected")
        producer = start("ffmpeg", [args.ffmpeg, "-nostdin", "-v", "error", "-re", "-f", "lavfi", "-i", "testsrc2=size=640x360:rate=30", "-t", "10", "-an", "-c:v", "libx264", "-preset", "ultrafast", "-tune", "zerolatency", "-g", "30", "-listen", "1", "-f", "h264", f"tcp://127.0.0.1:{source_port}"])
        time.sleep(1)
        pub = start("publisher", [lk, "room", "publish", "tcp-test", "--identity", "tcp-publisher", "--publish", f"h264://127.0.0.1:{source_port}", "--publish-tone", "--duration", "11", "--stats-interval-ms", "500"])
        wait_event("publisher", pub, "connected")
        people = run("room", "participants", "tcp-test")["participants"]
        (root / "participants.json").write_text(json.dumps(people, indent=2), encoding="utf-8")
        publisher = next(p for p in people if p["identity"] == "tcp-publisher")
        video = next(t["sid"] for t in publisher["tracks"] if t.get("type") == "VIDEO")
        # Proto3 JSON may omit AUDIO, the zero/default TrackType enum value.
        audio = next(t["sid"] for t in publisher["tracks"] if t.get("type", "AUDIO") == "AUDIO")
        selected = start("selected", [lk, "room", "subscribe", "tcp-test", "--identity", "selected", "--subscribe-track", video, "--record", str(root / "selected-recording"), "--duration", "7", "--stats-interval-ms", "500"])
        wait_event("selected", selected, "connected")
        run("room", "mute-track", "tcp-test", "--identity", "tcp-publisher", "--sid", audio)
        time.sleep(1)
        run("room", "mute-track", "tcp-test", "--identity", "tcp-publisher", "--sid", audio, "--unmute")
        assert selected.wait(timeout=20) == 0
        assert pub.wait(timeout=20) == 0
        assert sub.wait(timeout=20) == 0
        assert producer.wait(timeout=10) == 0
        selected_stats = [e for e in events("selected") if e.get("event") == "track-summary"]
        assert selected_stats and all(e["kind"] == "video" for e in selected_stats), selected_stats
        assert selected_stats[0]["frames"] >= 30, selected_stats
        all_stats = [e for e in events("all") if e.get("event") == "track-summary"]
        assert {e["kind"] for e in all_stats} == {"video", "audio"}
        assert any(e["kind"] == "video" and e["frames"] >= 100 for e in all_stats)
        muted = [e["muted"] for e in events("publisher") if e.get("event") == "track-muted"]
        assert True in muted and False in muted, muted
        kicked = start("kicked", [lk, "room", "join", "tcp-test", "--identity", "kicked"])
        wait_event("kicked", kicked, "connected")
        run("room", "remove-participant", "tcp-test", "--identity", "kicked")
        assert kicked.wait(timeout=15) != 0
        run("room", "delete", "tcp-test")
        with metrics_lock:
            samples = [s for s in metrics_samples if "sfu_target_bps" in s]
        assert samples, "No transport diagnostic samples"
        assert all(s["video_encode_calls"] == 0 and s["video_decode_calls"] == 0 for s in samples)
        peaks = {key:max(s.get(key,0) for s in samples) for key in (
            "sfu_target_bps", "sfu_pacer_queue_ms", "sfu_pacer_queue_bytes", "sfu_bandwidth_pauses", "sfu_probe_feedback_packets")}
        native = {label:[e for e in events(label) if e.get("event")=="rtc-stats"] for label in ("publisher","all","selected")}
        assert all(native.values()), "Native stats callbacks missing"
        outbound = [r for e in native["publisher"] for r in e["stats"] if r["type"]=="outbound-rtp" and r.get("kind")=="video"]
        assert outbound and max(r.get("framesEncoded",0) for r in outbound)>0, "No actual native encoder statistics"
        for label in ("all","selected"):
            assert any(r.get("framesDecoded",0)>0 for e in native[label] for r in e["stats"] if r["type"]=="inbound-rtp"), "No native decoder statistics"
        assert all(r["type"] not in ("local-candidate","remote-candidate","certificate") for reports in native.values() for e in reports for r in e["stats"])
        (root / "native-stats.json").write_text(json.dumps(native,indent=2),encoding="utf-8")
        summary = {"ok": True, "selected": selected_stats, "all": all_stats,
                   "server_sha256":hashlib.sha256(Path(args.server).read_bytes()).hexdigest(),
                   "sdk_sha256":hashlib.sha256(Path(lk).read_bytes()).hexdigest(), "sampled_peaks":peaks,
                   "server_url":server_url, "external_server":bool(args.external_url),
                   "checks": ["tcp-h264", "selective-subscription", "mute-unmute", "server-disconnect"]}
        (root / "result.json").write_text(json.dumps(summary, indent=2), encoding="utf-8")
        print(f"PASS: TCP H.264, selective subscription, mute/unmute, disconnect; artifacts: {root}")
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
        for f in files:
            f.close()


if __name__ == "__main__":
    main()
