"""Black-box JSON RoomService comparison against a locally built Go reference.

Uses only the Python standard library; owns both isolated server processes.
This is a control-plane subset, not a claim of complete LiveKit compatibility.
Reports are saved even on mismatch. No request tokens or signing secrets are logged.
"""
import argparse
import base64
import contextlib
import datetime
import hashlib
import hmac
import json
import os
from pathlib import Path
import secrets
import socket
import subprocess
import time
import urllib.error
import urllib.request


def b64(value):
    return base64.urlsafe_b64encode(value).rstrip(b"=").decode()


def token(key, secret, grants):
    now = int(time.time())
    header = b64(b'{"alg":"HS256","typ":"JWT"}')
    body = b64(json.dumps({"iss": key, "sub": "parity-runner", "nbf": now - 10,
                           "exp": now + 3600, "video": grants}).encode())
    data = f"{header}.{body}"
    return data + "." + b64(hmac.new(secret.encode(), data.encode(), hashlib.sha256).digest())


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def request(url, method, payload, auth, content_type="application/json", verb="POST"):
    headers = {"Content-Type": content_type}
    if auth is not None:
        headers["Authorization"] = "Bearer " + auth
    body = payload if isinstance(payload, bytes) else json.dumps(payload).encode()
    req = urllib.request.Request(url + "/twirp/livekit.RoomService/" + method,
                                 data=body, headers=headers, method=verb)
    # Local dedicated instances must not be routed through a user's HTTP proxy.
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    try:
        response = opener.open(req, timeout=12)
    except urllib.error.HTTPError as error:
        response = error
    with response:
        raw = response.read()
        try:
            data = json.loads(raw)
        except (ValueError, UnicodeDecodeError):
            data = {"non_json_body": raw.decode(errors="replace")}
        return {"status": response.status, "body": data}


def field(data, name, default=None):
    # Both proto field names and their JSON lowerCamelCase spellings are legal.
    camel = name.split("_")[0] + "".join(x.title() for x in name.split("_")[1:])
    return data.get(name, data.get(camel, default))


def room_projection(data):
    return {name: field(data, name, default) for name, default in (
        ("name", ""), ("empty_timeout", 0), ("departure_timeout", 0),
        ("max_participants", 0), ("metadata", ""), ("num_participants", 0))}


def compare_value(result, projection):
    value = {"status": result["status"]}
    if result["status"] != 200:
        value["code"] = result["body"].get("code")
    elif projection == "room":
        value["room"] = room_projection(result["body"])
    elif projection == "rooms":
        value["rooms"] = sorted((room_projection(r) for r in result["body"].get("rooms", [])),
                                key=lambda r: r["name"])
    elif projection == "participants":
        value["participants"] = result["body"].get("participants", [])
    else:
        value["body"] = result["body"]
    return value


def compact(value):
    """Keep failures inspectable without writing megabytes of metadata per case."""
    if isinstance(value, str) and len(value) > 1024:
        return {"utf8_bytes": len(value.encode()), "sha256": hashlib.sha256(value.encode()).hexdigest()}
    if isinstance(value, dict):
        return {k: compact(v) for k, v in value.items()}
    if isinstance(value, list):
        return [compact(v) for v in value]
    return value


@contextlib.contextmanager
def server(binary, kind, output, key, secret):
    port = free_port()
    config = output / (kind + (".yaml" if kind == "go" else ".json"))
    # There are no media participants in this suite. Separate media ranges keep
    # server initialization isolated; all HTTP and ICE endpoints are loopback.
    if kind == "go":
        config.write_text(f"port: {port}\nbind_addresses: ['127.0.0.1']\n"
                          "rtc:\n  tcp_port: 0\n  port_range_start: 59100\n"
                          "  port_range_end: 59120\n  use_external_ip: false\n"
                          f"  node_ip: 127.0.0.1\nkeys:\n  {key}: {secret}\n", encoding="utf-8")
    else:
        config.write_text(json.dumps({"bind": "127.0.0.1", "port": port, "keys": {key: secret},
                                      "rtc": {"port_range_start": 59130, "port_range_end": 59150}}),
                          encoding="utf-8")
    flags = subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0
    with (output / f"{kind}.log").open("wb") as log:
        process = subprocess.Popen([str(binary), "--config", str(config)], stdout=log,
                                   stderr=subprocess.STDOUT, creationflags=flags)
        try:
            url = f"http://127.0.0.1:{port}"
            deadline = time.monotonic() + 30
            auth = token(key, secret, {"roomList": True})
            while time.monotonic() < deadline:
                if process.poll() is not None:
                    raise RuntimeError(f"{kind} server exited with {process.returncode}; see {kind}.log")
                try:
                    response = request(url, "ListRooms", {}, auth)
                    if response["status"] == 200:
                        break
                except (OSError, urllib.error.URLError):
                    pass
                time.sleep(0.1)
            else:
                raise RuntimeError(f"{kind} server did not become ready")
            yield url
        finally:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=10)
            config.unlink(missing_ok=True)  # Ephemeral credentials never become artifacts.


def run_cases(urls, key, secret, report):
    name = "parity-" + secrets.token_hex(6)
    missing = name + "-missing"
    auth = {
        "admin": token(key, secret, {"roomCreate": True, "roomList": True, "roomAdmin": True, "room": name}),
        "none": token(key, secret, {}),
        "missing": token(key, secret, {"roomAdmin": True, "room": missing}),
        "other": token(key, secret, {"roomAdmin": True, "room": name + "-other"}),
    }
    seen_sid = {}

    def case(label, method, payload, projection="empty", grant="admin", expected_status=200, **kwargs):
        results = {}
        failures = []
        for kind, url in urls.items():
            try:
                result = request(url, method, payload, auth.get(grant), **kwargs)
                result["comparison"] = compare_value(result, projection)
                if result["status"] == 200 and projection == "room":
                    sid = result["body"].get("sid", "")
                    if not sid.startswith("RM_"):
                        failures.append(f"{kind}: missing room SID")
                    if field(result["body"], "name") == name:
                        if kind in seen_sid and seen_sid[kind] != sid:
                            failures.append(f"{kind}: existing room SID changed")
                        seen_sid[kind] = sid
                results[kind] = result
            except Exception as error:
                failures.append(f"{kind}: {type(error).__name__}: {error}")
        if len(results) == 2:
            if results["go"]["status"] != expected_status:
                failures.append(f"reference status {results['go']['status']} != expected {expected_status}")
            if results["go"]["comparison"] != results["cpp"]["comparison"]:
                failures.append("Go/C++ behavior differs")
        passed = not failures
        report["cases"].append({"name": label, "method": method, "projection": projection,
                                "passed": passed, "failures": failures, "results": compact(results)})
        print(("PASS " if passed else "FAIL ") + label, flush=True)

    case("create defaults", "CreateRoom", {"name": name}, "room")
    case("list filter", "ListRooms", {"names": [name]}, "rooms")
    case("create existing room with new settings", "CreateRoom",
         {"name": name, "emptyTimeout": 600, "departureTimeout": 40,
          "maxParticipants": 8, "metadata": "updated"}, "room")
    case("create existing preserves omitted settings", "CreateRoom", {"name": name}, "room")
    case("create existing ignores zero settings", "CreateRoom",
         {"name": name, "emptyTimeout": 0, "maxParticipants": 0, "metadata": ""}, "room")
    case("list updated room", "ListRooms", {"names": [name]}, "rooms")
    case("list unmatched", "ListRooms", {"names": [missing]}, "rooms")
    case("list unmatched JSON defaults", "ListRooms", {"names": [missing]})
    case("list participants empty", "ListParticipants", {"room": name}, "participants")
    case("list participants missing room", "ListParticipants", {"room": missing}, "participants", grant="missing")
    case("get missing participant", "GetParticipant", {"room": name, "identity": "nobody"}, expected_status=404)
    case("remove missing participant", "RemoveParticipant", {"room": name, "identity": "nobody"}, expected_status=404)
    case("metadata update", "UpdateRoomMetadata", {"room": name, "metadata": "hello"}, "room")
    case("metadata clear", "UpdateRoomMetadata", {"room": name, "metadata": ""}, "room")
    case("metadata 64 KiB plus one", "UpdateRoomMetadata", {"room": name, "metadata": "m" * 65537}, "room")
    case("metadata 512 KiB", "UpdateRoomMetadata", {"room": name, "metadata": "m" * (512 * 1024)}, "room")
    case("metadata escaped wire size", "UpdateRoomMetadata", {"room": name, "metadata": "\0" * (512 * 1024)}, "room")
    case("metadata over limit", "UpdateRoomMetadata", {"room": name, "metadata": "m" * (512 * 1024 + 1)}, expected_status=400)
    case("metadata clear after limit rejection", "UpdateRoomMetadata", {"room": name, "metadata": ""}, "room")
    case("send data without nonce", "SendData", {"room": name, "data": b64(b"test")})
    case("send data UUID nonce", "SendData", {"room": name, "data": b64(b"test"), "nonce": b64(bytes(16))})
    case("send data invalid nonce", "SendData", {"room": name, "data": b64(b"test"), "nonce": b64(b"bad")}, expected_status=400)
    case("send data unknown field", "SendData", {"room": name, "data": b64(b"test"), "futureField": 1})
    case("list unknown field", "ListRooms", {"names": [name], "futureField": 1}, "rooms")
    case("malformed JSON", "ListRooms", b"{", expected_status=400)
    case("invalid proto field type", "ListRooms", {"names": 42}, expected_status=400)
    # Observed Go behavior: empty names are accepted by CreateRoom even though
    # RTC admission has its own requirements. Preserve that reference behavior.
    case("empty room name", "CreateRoom", {}, "room")
    case("delete empty name room", "DeleteRoom", {"room": ""})
    case("room name too long", "CreateRoom", {"name": "x" * 257}, expected_status=400)
    case("list without permission", "ListRooms", {}, grant="none", expected_status=401)
    case("create without permission", "CreateRoom", {"name": name}, grant="none", expected_status=401)
    case("admin wrong room", "ListParticipants", {"room": name}, grant="other", expected_status=401)
    case("unknown method", "UnknownParityMethod", {}, expected_status=404)
    case("delete room", "DeleteRoom", {"room": name})
    case("list deleted room", "ListRooms", {"names": [name]}, "rooms")
    case("delete missing room", "DeleteRoom", {"room": name}, expected_status=404)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--go-server", type=Path, required=True)
    parser.add_argument("--cpp-server", type=Path, required=True)
    parser.add_argument("--output", type=Path, default=Path("tests/results/go-room-api-parity"))
    args = parser.parse_args()
    # A fresh output directory protects previous failure evidence.
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    output = (args.output / (stamp + "-" + secrets.token_hex(3))).resolve()
    output.mkdir(parents=True)
    report = {"suite": "JSON RoomService control-plane subset", "started_at_utc": stamp,
              "full_replacement_verified": False, "binaries": {}, "cases": []}
    status = 2
    try:
        binaries = {"go": args.go_server.resolve(strict=True), "cpp": args.cpp_server.resolve(strict=True)}
        for kind, binary in binaries.items():
            with binary.open("rb") as stream:
                digest = hashlib.file_digest(stream, "sha256").hexdigest()
            report["binaries"][kind] = {"path": str(binary), "sha256": digest}
        key, secret = "paritykey", secrets.token_hex(32)
        with contextlib.ExitStack() as stack:
            urls = {kind: stack.enter_context(server(binary, kind, output, key, secret))
                    for kind, binary in binaries.items()}
            run_cases(urls, key, secret, report)
        status = 0 if report["cases"] and all(c["passed"] for c in report["cases"]) else 1
    except Exception as error:
        report["infrastructure_error"] = f"{type(error).__name__}: {error}"
        print(report["infrastructure_error"], flush=True)
    finally:
        report["passed"] = sum(c["passed"] for c in report["cases"])
        report["total"] = len(report["cases"])
        report["exit_code"] = status
        (output / "report.json").write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
        print(f"{report['passed']}/{report['total']} passed; {output / 'report.json'}", flush=True)
    return status


if __name__ == "__main__":
    raise SystemExit(main())
