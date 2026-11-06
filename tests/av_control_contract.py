"""Headless HTTP/TLS contract tests for the AV media-control boundary.

Runs an isolated SFU and an in-process admission stub. No desktop, real capture
devices, production databases, or pre-existing media processes are touched.
"""
from __future__ import annotations

import argparse
import base64
import hashlib
import hmac
import http.server
import json
import os
from pathlib import Path
import shutil
import socket
import ssl
import subprocess
import tempfile
import threading
import time
import urllib.error
import urllib.request


def free_port() -> int:
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def token(issuer: str, secret: str, fields: dict) -> str:
    def encode(value: dict) -> bytes:
        return base64.urlsafe_b64encode(json.dumps(value, separators=(",", ":")).encode()).rstrip(b"=")
    now = int(time.time())
    payload = {"iss": issuer, "nbf": now - 1, "exp": now + 60, **fields}
    body = encode({"alg": "HS256", "typ": "JWT"}) + b"." + encode(payload)
    signature = base64.urlsafe_b64encode(hmac.new(secret.encode(), body, hashlib.sha256).digest()).rstrip(b"=")
    return (body + b"." + signature).decode()


def request(base: str, path: str, credential: str = "", body: dict | None = None) -> tuple[int, object]:
    headers = {"Content-Type": "application/json"}
    if credential:
        headers["Authorization"] = "Bearer " + credential
    req = urllib.request.Request(base + path, headers=headers,
                                 data=None if body is None else json.dumps(body).encode())
    try:
        response = urllib.request.urlopen(req, timeout=5)
    except urllib.error.HTTPError as error:
        response = error
    raw = response.read().decode()
    try:
        value = json.loads(raw)
    except json.JSONDecodeError:
        value = raw
    return response.status, value


CERTIFICATE_GENERATOR = r'''
package main
import("crypto/ecdsa";"crypto/elliptic";"crypto/rand";"crypto/x509";"crypto/x509/pkix";"encoding/pem";"math/big";"net";"os";"path/filepath";"time")
func main(){
 key,e:=ecdsa.GenerateKey(elliptic.P256(),rand.Reader);if e!=nil{panic(e)}
 cert:=&x509.Certificate{SerialNumber:big.NewInt(1),Subject:pkix.Name{CommonName:"localhost"},NotBefore:time.Now().Add(-time.Hour),NotAfter:time.Now().Add(time.Hour),KeyUsage:x509.KeyUsageDigitalSignature|x509.KeyUsageCertSign,ExtKeyUsage:[]x509.ExtKeyUsage{x509.ExtKeyUsageServerAuth},BasicConstraintsValid:true,IsCA:true,DNSNames:[]string{"localhost"},IPAddresses:[]net.IP{net.ParseIP("127.0.0.1")}}
 der,e:=x509.CreateCertificate(rand.Reader,cert,cert,&key.PublicKey,key);if e!=nil{panic(e)}
 raw,e:=x509.MarshalECPrivateKey(key);if e!=nil{panic(e)}
 if e=os.WriteFile(filepath.Join(os.Args[1],"cert.pem"),pem.EncodeToMemory(&pem.Block{Type:"CERTIFICATE",Bytes:der}),0600);e!=nil{panic(e)}
 if e=os.WriteFile(filepath.Join(os.Args[1],"key.pem"),pem.EncodeToMemory(&pem.Block{Type:"EC PRIVATE KEY",Bytes:raw}),0600);e!=nil{panic(e)}
}
'''


def run(binary: Path, go_binary: str | None, secure: bool) -> None:
    rtc_secret = "rtc-test-secret-" + "r" * 32
    management_secret = "management-test-secret-" + "m" * 32
    admission_secret = "admission-test-secret-" + "a" * 32
    state = {"mode": "allow", "calls": []}

    class Admission(http.server.BaseHTTPRequestHandler):
        def log_message(self, *_args):
            pass

        def do_POST(self):
            value = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
            state["calls"].append(value)
            authorized = self.headers.get("Authorization") == "Bearer " + admission_secret
            result = {"allowed": authorized and state["mode"] != "deny",
                      "policy_version": value["policy_version"],
                      "binding_generation": value["binding_generation"],
                      "expires_at": int(time.time()) + 120}
            if state["mode"] == "wrong_version":
                result["policy_version"] += 1
            elif state["mode"] == "empty":
                result = {}
            encoded = json.dumps(result).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(encoded)))
            self.end_headers()
            self.wfile.write(encoded)

    with tempfile.TemporaryDirectory(prefix="av-media-contract-") as directory:
        temp = Path(directory)
        admission = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Admission)
        if secure:
            if not go_binary:
                raise RuntimeError("TLS test needs --go pointing to Go for temporary certificate generation")
            generator = temp / "cert.go"
            generator.write_text(CERTIFICATE_GENERATOR, encoding="utf-8")
            subprocess.run([go_binary, "run", str(generator), str(temp)], check=True,
                           capture_output=True, creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
            context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            context.load_cert_chain(temp / "cert.pem", temp / "key.pem")
            admission.socket = context.wrap_socket(admission.socket, server_side=True)
        thread = threading.Thread(target=admission.serve_forever, daemon=True)
        thread.start()
        media_port = free_port()
        config = temp / "media.json"
        config.write_text(json.dumps({"bind": "127.0.0.1", "port": media_port,
                                      "keys": {"a-management": management_secret, "z-rtc": rtc_secret}}),
                          encoding="utf-8")
        env = {**os.environ, "AV_MEDIA_MODE": "true", "AV_NODE_ID": "contract-node",
               "AV_RTC_KEY": "z-rtc", "AV_MANAGEMENT_KEY": "a-management",
               "AV_ADMISSION_SECRET": admission_secret,
               "AV_ADMISSION_URL": f"{'https' if secure else 'http'}://127.0.0.1:{admission.server_port}/check",
               "AV_ALLOW_INSECURE_ADMISSION": "false" if secure else "true",
               "AV_ADMISSION_CA_FILE": str(temp / "cert.pem") if secure else ""}
        env.pop("LIVEKIT_KEYS", None)
        env.pop("LIVEKIT_CONFIG", None)
        log = (temp / "media.log").open("w", encoding="utf-8")
        process = subprocess.Popen([str(binary), "--config", str(config)], env=env,
                                   stdout=log, stderr=subprocess.STDOUT,
                                   creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        base = f"http://127.0.0.1:{media_port}"
        try:
            for _ in range(100):
                if process.poll() is not None:
                    raise RuntimeError("SFU exited: " + (temp / "media.log").read_text(encoding="utf-8"))
                try:
                    if request(base, "/healthz")[0] == 200:
                        break
                except (OSError, urllib.error.URLError):
                    time.sleep(0.1)
            else:
                raise RuntimeError("SFU startup timeout")
            admin = token("a-management", management_secret,
                          {"video": {"roomCreate": True, "roomList": True,
                                     "roomAdmin": True, "room": "contract-room"}})
            status, node = request(base, "/internal/v1/media/node", admin)
            assert status == 200 and node["ready"] and node["node_incarnation"]
            assert request(base, "/internal/v1/media/node")[0] == 401
            operation = {"operation_id": "create", "node_id": node["node_id"],
                         "node_incarnation": node["node_incarnation"], "room": "contract-room",
                         "binding_generation": 1, "target_policy_revision": 1,
                         "method": "CreateRoom", "payload": {"name": "contract-room"}}
            status, created = request(base, "/internal/v1/media/operations", admin, operation)
            assert status == 200, created
            assert request(base, "/internal/v1/media/operations", admin, operation) == (200, created)
            assert request(base, "/twirp/livekit.RoomService/CreateRoom", admin,
                           {"name": "unfenced"})[0] == 403
            fields = {"sub": "participant-one", "av_grant": "grant-one", "av_policy_version": 1,
                      "av_binding_generation": 1, "av_node_id": node["node_id"],
                      "av_node_incarnation": node["node_incarnation"],
                      "av_grant_expires_at": int(time.time()) + 300,
                      "video": {"roomJoin": True, "room": "contract-room", "canPublish": False}}
            rtc = token("z-rtc", rtc_secret, fields)
            assert request(base, "/rtc/validate", rtc)[0] == 200
            assert state["calls"][-1]["grant_id"] == "grant-one"
            for mode in ("deny", "wrong_version", "empty"):
                state["mode"] = mode
                assert request(base, "/rtc/validate", rtc)[0] == 401, mode
            state["mode"] = "allow"
            missing = dict(fields)
            missing.pop("av_grant")
            assert request(base, "/rtc/validate", token("z-rtc", rtc_secret, missing))[0] == 401
            wrong = {**fields, "av_node_incarnation": "old-boot"}
            assert request(base, "/rtc/validate", token("z-rtc", rtc_secret, wrong))[0] == 401
            assert request(base, "/rtc/validate", token("a-management", management_secret, fields))[0] == 401
            assert request(base, "/internal/v1/media/operations", rtc, operation)[0] == 403
            bad_operation = {**operation, "node_incarnation": "old-boot"}
            assert request(base, "/internal/v1/media/operations", admin, bad_operation)[0] == 409
            deletion = {**operation, "operation_id": "delete", "target_policy_revision": 2,
                        "method": "DeleteRoom", "payload": {"room": "contract-room"}}
            assert request(base, "/internal/v1/media/operations", admin, deletion)[0] == 200
            assert request(base, "/internal/v1/media/operations", admin, deletion)[0] == 200
            resurrection = {**operation, "operation_id": "late-create", "target_policy_revision": 3}
            assert request(base, "/internal/v1/media/operations", admin, resurrection)[0] == 409
            print(f"AV {'HTTPS' if secure else 'HTTP'} admission and fenced management contracts passed")
        finally:
            process.terminate()
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=5)
            log.close()
            admission.shutdown()
            admission.server_close()
            thread.join(timeout=5)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--go", default=shutil.which("go"))
    parser.add_argument("--tls", action="store_true")
    arguments = parser.parse_args()
    run(arguments.binary.resolve(), arguments.go, arguments.tls)
